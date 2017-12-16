#pragma once

#include <stdint.h>
#include <cstdlib>
#include <string>
#include <memory>
#include <option.h>
#include <rocksdb/db.h>
#include <rocksdb/options.h>
#include <rocksdb/merge_operator.h>
#include <rocksdb/transaction_log.h>

class UInt64AddOperator : public rocksdb::AssociativeMergeOperator {
public:
    virtual bool Merge(const rocksdb::Slice& key, const rocksdb::Slice* existing_value, const rocksdb::Slice& value,
                       std::string* new_value, rocksdb::Logger* logger) const override {
        uint64_t existing = 0;
        if (existing_value) {
            existing = (uint64_t) std::stoi(existing_value->ToString());
        }
        *new_value = std::to_string(existing + std::stoi(value.ToString()));
        return true;
    }

    virtual const char* Name() const override {
        return "UInt64AddOperator";
    }
};

enum StoreStatus {
    FOUND,
    OK,
    NOT_FOUND,
    ERROR
};

/*
 *  Abstraction for underlying KV store (RocksDB)
 */
class Store {
private:

    const std::string state_dir_path;

    rocksdb::DB *db;
    rocksdb::Options options;

public:

    Store() = delete;

    Store(const std::string & state_dir_path): state_dir_path(state_dir_path) {
        // Optimize RocksDB
        options.IncreaseParallelism();
        options.OptimizeLevelStyleCompaction();
        // create the DB if it's not already present
        options.create_if_missing = true;
        options.write_buffer_size = 4*1048576;
        options.max_write_buffer_number = 2;
        options.merge_operator.reset(new UInt64AddOperator);

        // these need to be high for replication scenarios
        options.WAL_ttl_seconds = 24*60*60;
        options.WAL_size_limit_MB = 1024;

        // open DB
        rocksdb::Status s = rocksdb::DB::Open(options, state_dir_path, &db);
        if(!s.ok()) {
            std::cerr << s.ToString() << std::endl;
        }
        assert(s.ok());
    }

    ~Store() {
        close();
    }

    bool insert(const std::string& key, const std::string& value) {
        rocksdb::Status status = db->Put(rocksdb::WriteOptions(), key, value);
        return status.ok();
    }

    bool contains(const std::string& key) const {
        std::string value;
        rocksdb::Status status = db->Get(rocksdb::ReadOptions(), key, &value);
        return status.ok() && !status.IsNotFound();
    }

    StoreStatus get(const std::string& key, std::string& value) const {
        rocksdb::Status status = db->Get(rocksdb::ReadOptions(), key, &value);

        if(status.IsNotFound()) {
            return StoreStatus::NOT_FOUND;
        }

        if(!status.ok()) {
            return StoreStatus::ERROR;
        }

        return StoreStatus::FOUND;
    }

    bool remove(const std::string& key) {
        rocksdb::Status status = db->Delete(rocksdb::WriteOptions(), key);
        return status.ok();
    }

    rocksdb::Iterator* scan(const std::string & prefix) {
        rocksdb::Iterator *iter = db->NewIterator(rocksdb::ReadOptions());
        iter->Seek(prefix);
        return iter;
    }

    rocksdb::Iterator* get_iterator() {
        rocksdb::Iterator* it = db->NewIterator(rocksdb::ReadOptions());
        return it;
    };

    void scan_fill(const std::string & prefix, std::vector<std::string> & values) {
        rocksdb::Iterator *iter = db->NewIterator(rocksdb::ReadOptions());
        for (iter->Seek(prefix); iter->Valid() && iter->key().starts_with(prefix); iter->Next()) {
            values.push_back(iter->value().ToString());
        }

        delete iter;
    }

    void increment(const std::string & key, uint32_t value) {
        db->Merge(rocksdb::WriteOptions(), key, std::to_string(value));
    }

    uint64_t get_latest_seq_number() const {
        return db->GetLatestSequenceNumber();
    }

    /*
       Since: GetUpdatesSince(0) == GetUpdatesSince(1), always query for 1 sequence number greater than the number
       returned by GetLatestSequenceNumber() locally.
     */
    Option<std::vector<std::string>*> get_updates_since(const uint64_t seq_number, const uint64_t max_updates) const {
        rocksdb::unique_ptr<rocksdb::TransactionLogIterator> iter;

        if(seq_number == db->GetLatestSequenceNumber()+1) {
            std::vector<std::string>* updates = new std::vector<std::string>();
            return Option<std::vector<std::string>*>(updates);
        }

        rocksdb::Status status = db->GetUpdatesSince(seq_number, &iter);
        if(!status.ok()) {
            return Option<std::vector<std::string>*>(204, "Invalid sequence number.");
        }

        uint64_t num_updates = 0;
        std::vector<std::string>* updates = new std::vector<std::string>();

        while(iter->Valid() && num_updates < max_updates) {
            rocksdb::BatchResult batch_result = iter->GetBatch();
            const std::string & write_batch_serialized = batch_result.writeBatchPtr->Data();
            updates->push_back(write_batch_serialized);
            num_updates += 1;
            iter->Next();
        }

        return Option<std::vector<std::string>*>(updates);
    }

    void close() {
        delete db;
        db = nullptr;
    }

    // Only for internal tests
    rocksdb::DB* _get_db_unsafe() const {
        return db;
    }

    void print_memory_usage() {
        std::string index_usage;
        db->GetProperty("rocksdb.estimate-table-readers-mem", &index_usage);
        std::cout << "rocksdb.estimate-table-readers-mem: " << index_usage << std::endl;

        std::string memtable_usage;
        db->GetProperty("rocksdb.cur-size-all-mem-tables", &memtable_usage);
        std::cout << "rocksdb.cur-size-all-mem-tables: " << memtable_usage << std::endl;
    }
};