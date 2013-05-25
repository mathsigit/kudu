// Copyright (c) 2013, Cloudera, inc.
#ifndef KUDU_TABLET_LAYER_BASEDATA_H
#define KUDU_TABLET_LAYER_BASEDATA_H

#include <boost/noncopyable.hpp>
#include <gtest/gtest.h>
#include <string>
#include <tr1/memory>

#include "cfile/bloomfile.h"
#include "cfile/cfile_reader.h"

#include "common/iterator.h"
#include "common/schema.h"
#include "tablet/memrowset.h"
#include "util/env.h"
#include "util/memory/arena.h"
#include "util/slice.h"

namespace kudu {

namespace tablet {

using boost::ptr_vector;
using kudu::cfile::BloomFileReader;
using kudu::cfile::CFileIterator;
using kudu::cfile::CFileReader;
using std::tr1::shared_ptr;

// Set of CFiles which make up the base data for a single rowset
//
// All of these files have the same number of rows, and thus the positional
// indexes can be used to seek to corresponding entries in each.
class CFileSet : public std::tr1::enable_shared_from_this<CFileSet>,
                 boost::noncopyable {
public:
  class Iterator;

  CFileSet(Env *env, const string &dir, const Schema &schema);

  Status OpenAllColumns();
  Status OpenKeyColumns();

  virtual Iterator *NewIterator(const Schema &projection) const;
  Status CountRows(rowid_t *count) const;
  uint64_t EstimateOnDiskSize() const;

  // Determine the index of the given row key.
  Status FindRow(const void *key, rowid_t *idx) const;

  const Schema &schema() const { return schema_; }

  string ToString() const {
    return string("CFile base data in ") + dir_;
  }

  virtual Status CheckRowPresent(const RowSetKeyProbe &probe, bool *present) const;

  virtual ~CFileSet();

private:
  friend class Iterator;

  Status OpenColumns(size_t num_cols);
  Status OpenBloomReader();

  Status NewColumnIterator(size_t col_idx, CFileIterator **iter) const;

  Env *env_;
  const string dir_;
  const Schema schema_;

  vector<shared_ptr<CFileReader> > readers_;
  gscoped_ptr<BloomFileReader> bloom_reader_;
};


////////////////////////////////////////////////////////////

// Column-wise iterator implementation over a set of column files.
//
// This simply ties together underlying files so that they can be batched
// together, and iterated in parallel.
class CFileSet::Iterator : public ColumnwiseIterator, public boost::noncopyable {
public:

  virtual Status Init(ScanSpec *spec);

  virtual Status PrepareBatch(size_t *nrows);

  virtual Status InitializeSelectionVector(SelectionVector *sel_vec);

  virtual Status MaterializeColumn(size_t col_idx, ColumnBlock *dst);

  virtual Status FinishBatch();

  virtual bool HasNext() const {
    DCHECK(initted_);
    return cur_idx_ <= upper_bound_idx_;
  }

  virtual string ToString() const {
    return string("rowset iterator for ") + base_data_->ToString();
  }

  const Schema &schema() const {
    return projection_;
  }

  // Collect the IO statistics for each of the underlying columns.
  void GetIOStatistics(vector<CFileIterator::IOStatistics> *stats);

private:
  FRIEND_TEST(TestCFileSet, TestRangeScan);
  friend class CFileSet;

  Iterator(const shared_ptr<CFileSet const> &base_data,
           const Schema &projection) :
    base_data_(base_data),
    projection_(projection),
    initted_(false),
    prepared_count_(0)
  {
    CHECK_OK(base_data_->CountRows(&row_count_));
  }


  // Look for a predicate which can be converted into a range scan using the key
  // column's index. If such a predicate exists, remove it from the scan spec and
  // store it in member fields.
  Status PushdownRangeScanPredicate(ScanSpec *spec);

  Status SeekToOrdinal(rowid_t ord_idx);
  void Unprepare();

  // Prepare the given column if not already prepared.
  Status PrepareColumn(size_t col_idx);

  const shared_ptr<CFileSet const> base_data_;
  const Schema projection_;
  vector<size_t> projection_mapping_;

  // Iterator for the key column in the underlying data.
  gscoped_ptr<CFileIterator> key_iter_;
  ptr_vector<CFileIterator> col_iters_;

  bool initted_;

  size_t cur_idx_;
  size_t prepared_count_;

  // The total number of rows in the file
  rowid_t row_count_;

  // Upper and lower (inclusive) bounds for this iterator, in terms of ordinal row indexes.
  // Both of these bounds are _inclusive_, and are always set (even if there is no predicate).
  // If there is no predicate, then the bounds will be [0, row_count_-1]
  rowid_t lower_bound_idx_;
  rowid_t upper_bound_idx_;


  // The underlying columns are prepared lazily, so that if a column is never
  // materialized, it doesn't need to be read off disk.
  vector<bool> cols_prepared_;

};

} // namespace tablet
} // namespace kudu
#endif
