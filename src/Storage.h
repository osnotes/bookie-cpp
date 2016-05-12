#pragma once

#include <rocksdb/db.h>
#include <folly/futures/Future.h>
#include <folly/MPMCQueue.h>

#include <memory>
#include <thread>

#include "BookieConfig.h"

using namespace folly;
using rocksdb::Slice;

class Storage {
public:
    Storage(const BookieConfig& conf);
    ~Storage();

    Future<Unit> put(const Slice& key, const Slice& value);

private:
    void runJournal();

    rocksdb::DB* db_;
    const rocksdb::WriteOptions writeOptions_;

    typedef std::unique_ptr<Promise<Unit>> PromisePtr;

    MPMCQueue<PromisePtr> journalQueue_;

    std::thread journalThread_;
};

