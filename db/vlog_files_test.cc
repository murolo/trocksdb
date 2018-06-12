//  Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).
//
// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include "db/db_test_util.h"
#include "port/stack_trace.h"
#include "port/port.h"
#include "rocksdb/experimental.h"
#include "rocksdb/utilities/convenience.h"
#include "util/sync_point.h"
#include<chrono>
#include<thread>

namespace rocksdb {

// SYNC_POINT is not supported in released Windows mode.
#if !defined(ROCKSDB_LITE)

class DBVLogTest : public DBTestBase {
 public:
  // these copied from CompactionPickerTest
  const Comparator* ucmp_;
  InternalKeyComparator icmp_;
  Options options_;
  ImmutableCFOptions ioptions_;
  MutableCFOptions mutable_cf_options_;
  LevelCompactionPicker level_compaction_picker;
  std::string cf_name_;
  uint32_t file_num_;
  CompactionOptionsFIFO fifo_options_;
  std::unique_ptr<VersionStorageInfo> vstorage_;
  std::vector<std::unique_ptr<FileMetaData>> files_;
  // does not own FileMetaData
  std::unordered_map<uint32_t, std::pair<FileMetaData*, int>> file_map_;
  // input files to compaction process.
  std::vector<CompactionInputFiles> input_files_;
  int compaction_level_start_;

  DBVLogTest() : DBTestBase("/db_compaction_test"), ucmp_(BytewiseComparator()),
        icmp_(ucmp_),
        ioptions_(options_),
        mutable_cf_options_(options_),
        level_compaction_picker(ioptions_, &icmp_),
        cf_name_("dummy"),
        file_num_(1),
        vstorage_(nullptr) {
     fifo_options_.max_table_files_size = 1;
    mutable_cf_options_.RefreshDerivedOptions(ioptions_);
    ioptions_.db_paths.emplace_back("dummy",
                                    std::numeric_limits<uint64_t>::max());}

  void DeleteVersionStorage() {
    vstorage_.reset();
    files_.clear();
    file_map_.clear();
    input_files_.clear();
  }

  void NewVersionStorage(int num_levels, CompactionStyle style) {
    DeleteVersionStorage();
    options_.num_levels = num_levels;

    vstorage_.reset(new VersionStorageInfo(&icmp_, ucmp_, options_.num_levels,
                                           style, nullptr, false
#ifdef INDIRECT_VALUE_SUPPORT
    , static_cast<ColumnFamilyHandleImpl*>(db_->DefaultColumnFamily())->cfd()  // needs to be &c_f_d for AR compactions
#endif
    ));
    vstorage_->CalculateBaseBytes(ioptions_, mutable_cf_options_);
  }

  void UpdateVersionStorageInfo() {
    vstorage_->CalculateBaseBytes(ioptions_, mutable_cf_options_);
    vstorage_->UpdateFilesByCompactionPri(ioptions_.compaction_pri);
    vstorage_->UpdateNumNonEmptyLevels();
    vstorage_->GenerateFileIndexer();
    vstorage_->GenerateLevelFilesBrief();
    vstorage_->ComputeCompactionScore(ioptions_, mutable_cf_options_);
    vstorage_->GenerateLevel0NonOverlapping();
    vstorage_->ComputeFilesMarkedForCompaction();
    vstorage_->SetFinalized();
  }

  void SetUpForActiveRecycleTest();

};

class DBVLogTestWithParam
    : public DBTestBase,
      public testing::WithParamInterface<std::tuple<uint32_t, bool>> {
 public:
  DBVLogTestWithParam() : DBTestBase("/db_compaction_test") {
    max_subcompactions_ = std::get<0>(GetParam());
    exclusive_manual_compaction_ = std::get<1>(GetParam());
  }

  // Required if inheriting from testing::WithParamInterface<>
  static void SetUpTestCase() {}
  static void TearDownTestCase() {}

  uint32_t max_subcompactions_;
  bool exclusive_manual_compaction_;
};

class DBCompactionDirectIOTest : public DBVLogTest,
                                 public ::testing::WithParamInterface<bool> {
 public:
  DBCompactionDirectIOTest() : DBVLogTest() {}
};

namespace {

class FlushedFileCollector : public EventListener {
 public:
  FlushedFileCollector() {}
  ~FlushedFileCollector() {}

  virtual void OnFlushCompleted(DB* db, const FlushJobInfo& info) override {
    std::lock_guard<std::mutex> lock(mutex_);
    flushed_files_.push_back(info.file_path);
  }

  std::vector<std::string> GetFlushedFiles() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> result;
    for (auto fname : flushed_files_) {
      result.push_back(fname);
    }
    return result;
  }

  void ClearFlushedFiles() { flushed_files_.clear(); }

 private:
  std::vector<std::string> flushed_files_;
  std::mutex mutex_;
};

static const int kCDTValueSize = 1000;
static const int kCDTKeysPerBuffer = 4;
static const int kCDTNumLevels = 8;
Options DeletionTriggerOptions(Options options) {
  options.compression = kNoCompression;
  options.write_buffer_size = kCDTKeysPerBuffer * (kCDTValueSize + 24);
  options.min_write_buffer_number_to_merge = 1;
  options.max_write_buffer_number_to_maintain = 0;
  options.num_levels = kCDTNumLevels;
  options.level0_file_num_compaction_trigger = 1;
  options.target_file_size_base = options.write_buffer_size * 2;
  options.target_file_size_multiplier = 2;
  options.max_bytes_for_level_base =
      options.target_file_size_base * options.target_file_size_multiplier;
  options.max_bytes_for_level_multiplier = 2;
  options.disable_auto_compactions = false;
  return options;
}

bool HaveOverlappingKeyRanges(
    const Comparator* c,
    const SstFileMetaData& a, const SstFileMetaData& b) {
  if (c->Compare(a.smallestkey, b.smallestkey) >= 0) {
    if (c->Compare(a.smallestkey, b.largestkey) <= 0) {
      // b.smallestkey <= a.smallestkey <= b.largestkey
      return true;
    }
  } else if (c->Compare(a.largestkey, b.smallestkey) >= 0) {
    // a.smallestkey < b.smallestkey <= a.largestkey
    return true;
  }
  if (c->Compare(a.largestkey, b.largestkey) <= 0) {
    if (c->Compare(a.largestkey, b.smallestkey) >= 0) {
      // b.smallestkey <= a.largestkey <= b.largestkey
      return true;
    }
  } else if (c->Compare(a.smallestkey, b.largestkey) <= 0) {
    // a.smallestkey <= b.largestkey < a.largestkey
    return true;
  }
  return false;
}

// Identifies all files between level "min_level" and "max_level"
// which has overlapping key range with "input_file_meta".
void GetOverlappingFileNumbersForLevelCompaction(
    const ColumnFamilyMetaData& cf_meta,
    const Comparator* comparator,
    int min_level, int max_level,
    const SstFileMetaData* input_file_meta,
    std::set<std::string>* overlapping_file_names) {
  std::set<const SstFileMetaData*> overlapping_files;
  overlapping_files.insert(input_file_meta);
  for (int m = min_level; m <= max_level; ++m) {
    for (auto& file : cf_meta.levels[m].files) {
      for (auto* included_file : overlapping_files) {
        if (HaveOverlappingKeyRanges(
                comparator, *included_file, file)) {
          overlapping_files.insert(&file);
          overlapping_file_names->insert(file.name);
          break;
        }
      }
    }
  }
}

void VerifyCompactionResult(
    const ColumnFamilyMetaData& cf_meta,
    const std::set<std::string>& overlapping_file_numbers) {
#ifndef NDEBUG
  for (auto& level : cf_meta.levels) {
    for (auto& file : level.files) {
      assert(overlapping_file_numbers.find(file.name) ==
             overlapping_file_numbers.end());
    }
  }
#endif
}

const SstFileMetaData* PickFileRandomly(
    const ColumnFamilyMetaData& cf_meta,
    Random* rand,
    int* level = nullptr) {
  auto file_id = rand->Uniform(static_cast<int>(
      cf_meta.file_count)) + 1;
  for (auto& level_meta : cf_meta.levels) {
    if (file_id <= level_meta.files.size()) {
      if (level != nullptr) {
        *level = level_meta.level;
      }
      auto result = rand->Uniform(file_id);
      return &(level_meta.files[result]);
    }
    file_id -= static_cast<uint32_t>(level_meta.files.size());
  }
  assert(false);
  return nullptr;
}
}  // anonymous namespace

#if 0
// All the TEST_P tests run once with sub_compactions disabled (i.e.
// options.max_subcompactions = 1) and once with it enabled
TEST_P(DBVLogTestWithParam, CompactionDeletionTrigger) {
  for (int tid = 0; tid < 3; ++tid) {
    uint64_t db_size[2];
    Options options = DeletionTriggerOptions(CurrentOptions());
    options.max_subcompactions = max_subcompactions_;

    if (tid == 1) {
      // the following only disable stats update in DB::Open()
      // and should not affect the result of this test.
      options.skip_stats_update_on_db_open = true;
    } else if (tid == 2) {
      // third pass with universal compaction
      options.compaction_style = kCompactionStyleUniversal;
      options.num_levels = 1;
    }

    DestroyAndReopen(options);
    Random rnd(301);
    const int kTestSize = kCDTKeysPerBuffer * 1024;
    std::vector<std::string> values;
    for (int k = 0; k < kTestSize; ++k) {
      values.push_back(RandomString(&rnd, kCDTValueSize));
      ASSERT_OK(Put(Key(k), values[k]));
    }
    dbfull()->TEST_WaitForFlushMemTable();
    dbfull()->TEST_WaitForCompact();
    db_size[0] = Size(Key(0), Key(kTestSize - 1));

    for (int k = 0; k < kTestSize; ++k) {
      ASSERT_OK(Delete(Key(k)));
    }
    dbfull()->TEST_WaitForFlushMemTable();
    dbfull()->TEST_WaitForCompact();
    db_size[1] = Size(Key(0), Key(kTestSize - 1));

    // must have much smaller db size.
    ASSERT_GT(db_size[0] / 3, db_size[1]);
  }
}
#endif
static void ListVLogFileSizes(DBVLogTest *db, std::vector<uint64_t>& vlogfilesizes){
  vlogfilesizes.clear();
  // Get the list of file in the DB, cull that down to VLog files
  std::vector<std::string> filenames;
  ASSERT_OK(db->env_->GetChildren(db->db_->GetOptions().db_paths.back().path, &filenames));
  for (size_t i = 0; i < filenames.size(); i++) {
    uint64_t number;
    FileType type;
    if (ParseFileName(filenames[i], &number, &type)) {
      if (type == kVLogFile) {
        // vlog file.  Extract the CF id from the file extension
        size_t dotpos=filenames[i].find_last_of('.');  // find start of extension
        std::string cfname(filenames[i].substr(dotpos+1+kRocksDbVLogFileExt.size()));
        uint64_t file_size; db->env_->GetFileSize(db->db_->GetOptions().db_paths.back().path+"/"+filenames[i], &file_size);
        vlogfilesizes.emplace_back(file_size);
      }
    }
  }
}

TEST_F(DBVLogTest, VlogFileSizeTest) {
  Options options = CurrentOptions();
  options.write_buffer_size = 100 * 1024 * 1024;  // 100MB write buffer: hold all keys
  options.num_levels = 4;
  options.level0_file_num_compaction_trigger = 3;
  options.level0_slowdown_writes_trigger = 5;
  options.level0_stop_writes_trigger = 11;
  options.max_background_compactions = 3;
  options.max_bytes_for_level_base = 100 * (1LL<<20);  // keep level1 big too
  options.max_bytes_for_level_multiplier = 10;

  const int32_t key_size = 100;
  const int32_t value_size = (1LL<<14);  // k+v=16KB
  options.target_file_size_base = 100LL << 20;  // high limit for compaction result, so we don't subcompact

  options.vlogring_activation_level = std::vector<int32_t>({0});
  options.min_indirect_val_size = std::vector<size_t>({0});
  options.fraction_remapped_during_compaction = std::vector<int32_t>({50});
  options.fraction_remapped_during_active_recycling = std::vector<int32_t>({25});
  options.fragmentation_active_recycling_trigger = std::vector<int32_t>({25});
  options.fragmentation_active_recycling_klaxon = std::vector<int32_t>({50});
  options.active_recycling_sst_minct = std::vector<int32_t>({5});
  options.active_recycling_sst_maxct = std::vector<int32_t>({15});
  options.active_recycling_vlogfile_freed_min = std::vector<int32_t>({7});
  options.compaction_picker_age_importance = std::vector<int32_t>({100});
  options.ring_compression_style = std::vector<CompressionType>({kNoCompression});
  options.vlogfile_max_size = std::vector<uint64_t>({4LL << 20});  // 4MB


  Random rnd(301);
  // Key of 100 bytes, kv of 16KB
  // Set filesize to 4MB
  // Write keys 4MB+multiples of 1MB; Compact them to L1; count files & sizes.
  // # files should be correct, and files should be close to the same size
  for(size_t nkeys = options.vlogfile_max_size[0]/value_size; nkeys<10*options.vlogfile_max_size[0]/value_size; nkeys += (1LL<<20)/value_size){
// scaf for speedup printf("%zd\n",nkeys);    break; 
    DestroyAndReopen(options);
    // write the kvs
    for(int key = 0;key<nkeys;++key){
      ASSERT_OK(Put(Key(key), RandomString(&rnd, value_size)));
    }
    size_t vsize=nkeys*(value_size+5);  // bytes written to vlog.  5 bytes for compression header
    // compact them
    ASSERT_OK(Flush());
    // Compact them into L1, which will write the VLog files
    CompactRangeOptions compact_options;
    compact_options.change_level = true;
    compact_options.target_level = 1;
    ASSERT_OK(db_->CompactRange(compact_options, nullptr, nullptr));
    // 1 file in L1, after compaction
    ASSERT_EQ("0,1", FilesPerLevel(0));
    std::vector<uint64_t> vlogfilesizes;  // sizes of all the VLog files
    ListVLogFileSizes(this,vlogfilesizes);
    // verify number of files is correct
    uint64_t expfiles = (uint64_t)std::max(std::ceil(vsize/((double)options.vlogfile_max_size[0]*1.25)),std::floor(vsize/((double)options.vlogfile_max_size[0])));
    ASSERT_EQ(expfiles, vlogfilesizes.size());
    // verify files have almost the same size
    uint64_t minsize=~0, maxsize=0;  // place to build file stats
    for(size_t i=0;i<vlogfilesizes.size();++i){
     minsize=std::min(minsize,vlogfilesizes[i]); maxsize=std::max(maxsize,vlogfilesizes[i]);
    }
    ASSERT_LT(maxsize-minsize, 2*value_size);
  }
#if 0
  // Add 2 non-overlapping files
  std::map<int32_t, std::string> values;

  // file 1 [0 => 100]
  for (int32_t i = 0; i < 100; i++) {
    values[i] = RandomString(&rnd, value_size + rnd.Next()%value_size_var);
    ASSERT_OK(Put(LongKey(i,key_size), values[i]));
  }
  ASSERT_OK(Flush());

  // file 2 [100 => 300]
  for (int32_t i = 100; i < 300; i++) {
    values[i] = RandomString(&rnd, value_size + rnd.Next()%value_size_var);
    ASSERT_OK(Put(LongKey(i,key_size), values[i]));
  }
  ASSERT_OK(Flush());

  // 2 files in L0
  ASSERT_EQ("2", FilesPerLevel(0));
  CompactRangeOptions compact_options;
  compact_options.change_level = true;
  compact_options.target_level = 2;
  ASSERT_OK(db_->CompactRange(compact_options, nullptr, nullptr));
  // 2 files in L2
//  ASSERT_EQ("0,0,2", FilesPerLevel(0));

  // file 3 [ 0 => 200]
  for (int32_t i = 0; i < 200; i++) {
    values[i] = RandomString(&rnd, value_size + rnd.Next()%value_size_var);
    ASSERT_OK(Put(LongKey(i,key_size), values[i]));
  }
  ASSERT_OK(Flush());
  for (int32_t i = 300; i < 300+batch_size; i++) {
    std::string vstg =  RandomString(&rnd, value_size);
    // append compressible suffix
    vstg.append(rnd.Next()%value_size_var,'a');
    values[i] = vstg;
  }


for(int32_t k=0;k<10;++k) {
  // Many files 4 [300 => 4300)
  for (int32_t i = 0; i <= 5; i++) {
    for (int32_t j = 300; j < batch_size+300; j++) {
//      if (j == 2300) {
//        ASSERT_OK(Flush());
//        dbfull()->TEST_WaitForFlushMemTable();
//      }
      if((rnd.Next()&0x7f)==0)values[j] = RandomString(&rnd, value_size + rnd.Next()%value_size_var);  // replace one value in 100
      Status s = (Put(LongKey(j,key_size), values[j]));
      if(!s.ok())
        printf("Put failed\n");
      if(i|k) {   // if we have filled up all the slots...
        for(int32_t m=0;m<2;++m){
          int32_t randkey = (rnd.Next()) % batch_size;  // make 2 random gets per put
          std::string getresult = Get(LongKey(randkey,key_size));
          if(getresult.compare(values[randkey])!=0) {
            printf("mismatch: Get result=%s len=%zd\n",getresult.c_str(),getresult.size());
            printf("mismatch: Expected=%s len=%zd\n",values[randkey].c_str(),values[randkey].size());
            std::string getresult2 = Get(LongKey(randkey,key_size));
            if(getresult.compare(getresult2)==0)printf("unchanged on reGet\n");
            else if(getresult.compare(values[randkey])==0)printf("correct on reGet\n");
            else printf("after ReGet: Get result=%s len=%zd\n",getresult2.c_str(),getresult2.size());
          }
        }
      }
      
    }
    printf("batch ");
    std::this_thread::sleep_for(std::chrono::seconds(2));  // give the compactor time to run
  }
//  ASSERT_OK(Flush());
//  dbfull()->TEST_WaitForFlushMemTable();
//  dbfull()->TEST_WaitForCompact();

  for (int32_t j = 0; j < batch_size+300; j++) {
    ASSERT_EQ(Get(LongKey(j,key_size)), values[j]);
  }
  printf("...verified.\n");
  TryReopen(options);
  printf("reopened.\n");
}


  // Verify level sizes
  uint64_t target_size = 4 * options.max_bytes_for_level_base;
  for (int32_t i = 1; i < options.num_levels; i++) {
    ASSERT_LE(SizeAtLevel(i), target_size);
    target_size = static_cast<uint64_t>(target_size *
                                        options.max_bytes_for_level_multiplier);
  }

  size_t old_num_files = CountFiles();
  std::string begin_string = LongKey(1000,key_size);
  std::string end_string = LongKey(2000,key_size);
  Slice begin(begin_string);
  Slice end(end_string);
  ASSERT_OK(DeleteFilesInRange(db_, db_->DefaultColumnFamily(), &begin, &end));

  int32_t deleted_count = 0;
  for (int32_t i = 0; i < 4300; i++) {
    if (i < 1000 || i > 2000) {
      ASSERT_EQ(Get(LongKey(i,key_size)), values[i]);
    } else {
      ReadOptions roptions;
      std::string result;
      Status s = db_->Get(roptions, LongKey(i,key_size), &result);
      ASSERT_TRUE(s.IsNotFound() || s.ok());
      if (s.IsNotFound()) {
        deleted_count++;
      }
    }
  }
  ASSERT_GT(deleted_count, 0);
  begin_string = LongKey(5000,key_size);
  end_string = LongKey(6000,key_size);
  Slice begin1(begin_string);
  Slice end1(end_string);
  // Try deleting files in range which contain no keys
  ASSERT_OK(
      DeleteFilesInRange(db_, db_->DefaultColumnFamily(), &begin1, &end1));

  // Push data from level 0 to level 1 to force all data to be deleted
  // Note that we don't delete level 0 files
  compact_options.change_level = true;
  compact_options.target_level = 1;
  ASSERT_OK(dbfull()->TEST_CompactRange(0, nullptr, nullptr));

  ASSERT_OK(
      DeleteFilesInRange(db_, db_->DefaultColumnFamily(), nullptr, nullptr));

  int32_t deleted_count2 = 0;
  for (int32_t i = 0; i < 4300; i++) {
    ReadOptions roptions;
    std::string result;
    Status s = db_->Get(roptions, LongKey(i,key_size), &result);
    ASSERT_TRUE(s.IsNotFound());
    deleted_count2++;
  }
  ASSERT_GT(deleted_count2, deleted_count);
  size_t new_num_files = CountFiles();
  ASSERT_GT(old_num_files, new_num_files);
#endif
}

TEST_F(DBVLogTest, MinIndirectValSizeTest) {
  Options options = CurrentOptions();
  options.write_buffer_size = 100 * 1024 * 1024;  // 100MB write buffer: hold all keys
  options.num_levels = 4;
  options.level0_file_num_compaction_trigger = 3;
  options.level0_slowdown_writes_trigger = 5;
  options.level0_stop_writes_trigger = 11;
  options.max_background_compactions = 3;
  options.max_bytes_for_level_base = 100 * (1LL<<20);  // keep level1 big too
  options.max_bytes_for_level_multiplier = 10;

  const int32_t key_size = 100;
  const int32_t value_size_incr = 10;  // k+v=16KB
  const int32_t nkeys = 2000;  // k+v=16KB
  options.target_file_size_base = 100LL << 20;  // high limit for compaction result, so we don't subcompact

  options.vlogring_activation_level = std::vector<int32_t>({0});
  options.min_indirect_val_size = std::vector<size_t>({0});
  options.fraction_remapped_during_compaction = std::vector<int32_t>({50});
  options.fraction_remapped_during_active_recycling = std::vector<int32_t>({25});
  options.fragmentation_active_recycling_trigger = std::vector<int32_t>({25});
  options.fragmentation_active_recycling_klaxon = std::vector<int32_t>({50});
  options.active_recycling_sst_minct = std::vector<int32_t>({5});
  options.active_recycling_sst_maxct = std::vector<int32_t>({15});
  options.active_recycling_vlogfile_freed_min = std::vector<int32_t>({7});
  options.compaction_picker_age_importance = std::vector<int32_t>({100});
  options.ring_compression_style = std::vector<CompressionType>({kNoCompression});
  options.vlogfile_max_size = std::vector<uint64_t>({4LL << 20});  // 4MB

  Random rnd(301);
  // Key of 100 bytes, kv of 16KB
  // Set filesize to 4MB
  // Write keys 4MB+multiples of 1MB; Compact them to L1; count files & sizes.
  // total # bytes in files should go down as the minimum remapping size goes up
  for(size_t minsize=0; minsize<nkeys*value_size_incr; minsize+=500){
// scaf speedup printf("%zd\n",minsize); break;  
    options.min_indirect_val_size[0]=minsize;
    DestroyAndReopen(options);
    // write the kvs
    int value_size=0;  // we remember the ending value
    for(int key = 0;key<nkeys;++key){
      ASSERT_OK(Put(Key(key), RandomString(&rnd, value_size)));
      value_size += value_size_incr;
    }
    // compact them
    ASSERT_OK(Flush());
    // Compact them into L1, which will write the VLog files
    CompactRangeOptions compact_options;
    compact_options.change_level = true;
    compact_options.target_level = 1;
    ASSERT_OK(db_->CompactRange(compact_options, nullptr, nullptr));
    // 1 file in L1, after compaction
    ASSERT_EQ("0,1", FilesPerLevel(0));
    std::vector<uint64_t> vlogfilesizes;  // sizes of all the VLog files
    ListVLogFileSizes(this,vlogfilesizes);
    // verify total file size is correct for the keys we remapped
    int64_t totalsize=0;  // place to build file stats
    for(size_t i=0;i<vlogfilesizes.size();++i){
     totalsize += vlogfilesizes[i];
    }
    // mapped size = #mapped rcds * average size of rcd
    int64_t nmappedkeys = nkeys * (value_size - minsize) / value_size;
    int64_t expmappedsize = nmappedkeys * ((minsize + value_size)+5) / 2;  // 5 for compression header
    ASSERT_LT(std::abs(expmappedsize-totalsize), 2*value_size);
  }
}


TEST_F(DBVLogTest, VLogCompressionTest) {
  Options options = CurrentOptions();
  options.write_buffer_size = 100 * 1024 * 1024;  // 100MB write buffer: hold all keys
  options.num_levels = 4;
  options.level0_file_num_compaction_trigger = 3;
  options.level0_slowdown_writes_trigger = 5;
  options.level0_stop_writes_trigger = 11;
  options.max_background_compactions = 3;
  options.max_bytes_for_level_base = 100 * (1LL<<20);  // keep level1 big too
  options.max_bytes_for_level_multiplier = 10;

  const int32_t key_size = 100;
  const int32_t value_size_incr = 10;  // k+v=16KB
  const int32_t nkeys = 2000;  // k+v=16KB
  options.target_file_size_base = 100LL << 20;  // high limit for compaction result, so we don't subcompact

  options.vlogring_activation_level = std::vector<int32_t>({0});
  options.min_indirect_val_size = std::vector<size_t>({0});
  options.fraction_remapped_during_compaction = std::vector<int32_t>({50});
  options.fraction_remapped_during_active_recycling = std::vector<int32_t>({25});
  options.fragmentation_active_recycling_trigger = std::vector<int32_t>({25});
  options.fragmentation_active_recycling_klaxon = std::vector<int32_t>({50});
  options.active_recycling_sst_minct = std::vector<int32_t>({5});
  options.active_recycling_sst_maxct = std::vector<int32_t>({15});
  options.active_recycling_vlogfile_freed_min = std::vector<int32_t>({7});
  options.compaction_picker_age_importance = std::vector<int32_t>({100});
  options.ring_compression_style = std::vector<CompressionType>({kNoCompression});
  options.vlogfile_max_size = std::vector<uint64_t>({4LL << 20});  // 4MB

  Random rnd(301);
  // Key of 100 bytes, kv of 16KB
  // Set filesize to 4MB
  // Write keys 4MB+multiples of 1MB; Compact them to L1; count files & sizes.
  options.min_indirect_val_size[0]=0;
  DestroyAndReopen(options);
  // write the kvs
  int value_size=0;  // we remember the ending value
  for(int key = 0;key<nkeys;++key){
    std::string vstg(RandomString(&rnd, 10));
    vstg.resize(value_size,' ');  // extend string with compressible data
    ASSERT_OK(Put(Key(key), vstg));
    value_size += value_size_incr;
  }
  // compact them
  ASSERT_OK(Flush());
  // Compact them into L1, which will write the VLog files
  CompactRangeOptions compact_options;
  compact_options.change_level = true;
  compact_options.target_level = 1;
  ASSERT_OK(db_->CompactRange(compact_options, nullptr, nullptr));
  // 1 file in L1, after compaction
  ASSERT_EQ("0,1", FilesPerLevel(0));
  std::vector<uint64_t> vlogfilesizes;  // sizes of all the VLog files
  ListVLogFileSizes(this,vlogfilesizes);
  // verify total file size is correct for the keys we remapped
  int64_t totalsize=0;  // place to build file stats
  for(size_t i=0;i<vlogfilesizes.size();++i){
   totalsize += vlogfilesizes[i];
  }

  // Repeat with VLog compression on
  options.ring_compression_style = std::vector<CompressionType>({kSnappyCompression});

  DestroyAndReopen(options);
  // write the kvs
  value_size=0;  // we remember the ending value
  for(int key = 0;key<nkeys;++key){
    std::string vstg(RandomString(&rnd, 10));
    vstg.resize(value_size,' ');  // extend string with compressible data
    ASSERT_OK(Put(Key(key), vstg));
    value_size += value_size_incr;
  }
  // compact them
  ASSERT_OK(Flush());
  // Compact them into L1, which will write the VLog files
  ASSERT_OK(db_->CompactRange(compact_options, nullptr, nullptr));
  // 1 file in L1, after compaction
  ASSERT_EQ("0,1", FilesPerLevel(0));
  ListVLogFileSizes(this,vlogfilesizes);
  // verify total file size is correct for the keys we remapped
  int64_t totalsizecomp=0;  // place to build file stats
  for(size_t i=0;i<vlogfilesizes.size();++i){
   totalsizecomp += vlogfilesizes[i];
  }


  // Verify compression happened
  ASSERT_LT(totalsizecomp, 0.1*totalsize);
}


TEST_F(DBVLogTest, RemappingFractionTest) {
  Options options = CurrentOptions();
  options.write_buffer_size = 100 * 1024 * 1024;  // 100MB write buffer: hold all keys
  options.num_levels = 4;
  options.level0_file_num_compaction_trigger = 3;
  options.level0_slowdown_writes_trigger = 5;
  options.level0_stop_writes_trigger = 11;
  options.max_background_compactions = 3;
  options.max_bytes_for_level_base = 100 * (1LL<<20);  // keep level1 big too
  options.max_bytes_for_level_multiplier = 10;

  const int32_t key_size = 100;
  const int32_t value_size = 16384;  // k+v=16KB
  const int32_t nkeys = 100;  // number of files
  options.target_file_size_base = 100LL << 20;  // high limit for compaction result, so we don't subcompact

  options.vlogring_activation_level = std::vector<int32_t>({0});
  options.min_indirect_val_size = std::vector<size_t>({0});
  options.fraction_remapped_during_compaction = std::vector<int32_t>({50});
  options.fraction_remapped_during_active_recycling = std::vector<int32_t>({25});
  options.fragmentation_active_recycling_trigger = std::vector<int32_t>({25});
  options.fragmentation_active_recycling_klaxon = std::vector<int32_t>({50});
  options.active_recycling_sst_minct = std::vector<int32_t>({5});
  options.active_recycling_sst_maxct = std::vector<int32_t>({15});
  options.active_recycling_vlogfile_freed_min = std::vector<int32_t>({7});
  options.compaction_picker_age_importance = std::vector<int32_t>({100});
  options.ring_compression_style = std::vector<CompressionType>({kNoCompression});
  options.vlogfile_max_size = std::vector<uint64_t>({4LL << 20});  // 4MB

  Random rnd(301);
  // Key of 100 bytes, kv of 16KB
  // Set filesize to 4MB


  for(int32_t mappct=20; mappct<=40; mappct+= 5){
    double mapfrac = mappct * 0.01;
// scaf speedup printf("%f\n",mapfrac);   break;  
    // set remapping fraction to n
    options.fraction_remapped_during_compaction = std::vector<int32_t>({mappct});
    DestroyAndReopen(options);
    // write 100 files in key order, flushing each to give it a VLog file
    std::string val0string;
    for(int key = 0;key<nkeys;++key){
      std::string keystring = Key(key);
      Slice keyslice = keystring;  // the key we will add
      std::string valstring = RandomString(&rnd, value_size);
      if(key==0)val0string = valstring;
      Slice valslice = valstring;
      ASSERT_OK(Put(keyslice, valslice));
      ASSERT_EQ(valslice, Get(Key(key)));
      // Flush into L0
      ASSERT_OK(Flush());
      // Compact into L1, which will write the VLog files.  At this point there is nothing to remap
      CompactRangeOptions compact_options;
      compact_options.change_level = true;
      compact_options.target_level = 1;
      ASSERT_OK(db_->CompactRange(compact_options, &keyslice, &keyslice));
    }
    // Verify 100 SSTs and 100 VLog files
    ASSERT_EQ("0,100", FilesPerLevel(0));
    std::vector<uint64_t> vlogfilesizes;  // sizes of all the VLog files
    ListVLogFileSizes(this,vlogfilesizes);
    ASSERT_EQ(100, vlogfilesizes.size());
    // Verify total file size is pretty close to right
    int64_t totalsize=0;  // place to build file stats
    for(size_t i=0;i<vlogfilesizes.size();++i){
     totalsize += vlogfilesizes[i];
    }
    ASSERT_GT(1000, std::abs(totalsize-(value_size+5)*nkeys));
    // compact n-5% to n+5%
    CompactRangeOptions remap_options;
    remap_options.change_level = true;
    remap_options.target_level = 1;
    remap_options.bottommost_level_compaction = BottommostLevelCompaction::kForce;
    ASSERT_OK(db_->CompactRange(remap_options, &Slice(Key((int)(nkeys*(mapfrac-0.05)))), &Slice(Key((int)(nkeys*(mapfrac+0.05))))));
    // verify that the VLog file total has grown by 5%
    vlogfilesizes.clear();  // reinit list of files
    ListVLogFileSizes(this,vlogfilesizes);
    int64_t newtotalsize=0;  // place to build file stats
    for(size_t i=0;i<vlogfilesizes.size();++i){
     newtotalsize += vlogfilesizes[i];
    }
    ASSERT_GT(value_size+500, (int64_t)std::abs(newtotalsize-totalsize*1.05));  // not a tight match, but if it works throughout the range it's OK
  }
}

void DBVLogTest::SetUpForActiveRecycleTest() {
  Options options = CurrentOptions();
  options.write_buffer_size = 100 * 1024 * 1024;  // 100MB write buffer: hold all keys
  options.num_levels = 4;
  options.level0_file_num_compaction_trigger = 3;
  options.level0_slowdown_writes_trigger = 5;
  options.level0_stop_writes_trigger = 11;
  options.max_background_compactions = 3;
  options.max_bytes_for_level_base = 100 * (1LL<<20);  // keep level1 big too
  options.max_bytes_for_level_multiplier = 10;

  const int32_t key_size = 100;
  const int32_t value_size = 16384;  // k+v=16KB
  const int32_t nkeys = 100;  // number of files
  options.target_file_size_base = 100LL << 20;  // high limit for compaction result, so we don't subcompact

  options.vlogring_activation_level = std::vector<int32_t>({0});
  options.min_indirect_val_size = std::vector<size_t>({0});
  options.fraction_remapped_during_compaction = std::vector<int32_t>({50});
  options.fraction_remapped_during_active_recycling = std::vector<int32_t>({25});
  options.fragmentation_active_recycling_trigger = std::vector<int32_t>({25});
  options.fragmentation_active_recycling_klaxon = std::vector<int32_t>({50});
  options.active_recycling_sst_minct = std::vector<int32_t>({5});
  options.active_recycling_sst_maxct = std::vector<int32_t>({15});
  options.active_recycling_vlogfile_freed_min = std::vector<int32_t>({7});
  options.compaction_picker_age_importance = std::vector<int32_t>({100});
  options.ring_compression_style = std::vector<CompressionType>({kNoCompression});
  options.vlogfile_max_size = std::vector<uint64_t>({4LL << 20});  // 4MB

  Random rnd(301);
  // Key of 100 bytes, kv of 16KB
  // Set filesize to 4MB

  // set up 100 files in L1
  // set AR level to 10%
  // compact & remap 15% of the data near the beginning of the database
  // AR should happen, freeing the beginning of the db

  int32_t mappct=20;
  double mapfrac = mappct * 0.01;
// scaf speedup printf("%f\n",mapfrac);  break;
  // turn off remapping
  options.fraction_remapped_during_compaction = std::vector<int32_t>({mappct});
  options.fragmentation_active_recycling_trigger = std::vector<int32_t>({10});
  options.active_recycling_size_trigger = std::vector<int64_t>({0});

  DestroyAndReopen(options);
  // write 100 files in key order, flushing each to give it a VLog file
  std::string val0string;
  for(int key = 0;key<nkeys;++key){
    std::string keystring = Key(key);
    Slice keyslice = keystring;  // the key we will add
    std::string valstring = RandomString(&rnd, value_size);
    if(key==0)val0string = valstring;
    Slice valslice = valstring;
    ASSERT_OK(Put(keyslice, valslice));
    ASSERT_EQ(valslice, Get(Key(key)));
    // Flush into L0
    ASSERT_OK(Flush());
    // Compact into L1, which will write the VLog files.  At this point there is nothing to remap
    CompactRangeOptions compact_options;
    compact_options.change_level = true;
    compact_options.target_level = 1;
    ASSERT_OK(db_->CompactRange(compact_options, &keyslice, &keyslice));
  }
  // Verify 100 SSTs and 100 VLog files
  ASSERT_EQ("0,100", FilesPerLevel(0));
  std::vector<uint64_t> vlogfilesizes;  // sizes of all the VLog files
  ListVLogFileSizes(this,vlogfilesizes);
  ASSERT_EQ(100, vlogfilesizes.size());
  // Verify total file size is pretty close to right
  int64_t totalsize=0;  // place to build file stats
  for(size_t i=0;i<vlogfilesizes.size();++i){
   totalsize += vlogfilesizes[i];
  }
  ASSERT_GT(1000, std::abs(totalsize-(value_size+5)*nkeys));
  // compact between 5% and 20%.  This should remap all, leaving 15% fragmentation.
  CompactRangeOptions remap_options;
  remap_options.change_level = true;
  remap_options.target_level = 1;
  remap_options.bottommost_level_compaction = BottommostLevelCompaction::kForce;
  remap_options.exclusive_manual_compaction = false;
  ASSERT_OK(db_->CompactRange(remap_options, &Slice(Key((int)(nkeys*0.05))), &Slice(Key((int)(nkeys*0.20)))));
  // The user compaction and the AR compaction should both run.
  // scaf  printf("%s\n", FilesPerLevel(0).c_str());

  NewVersionStorage(6, kCompactionStyleLevel);
  UpdateVersionStorageInfo();

  // Now we have 5 VLog files, numbered 1-5, holding keys 0-4, referred to by SSTs 0-4
  // then files 6-19, which have no reference
  // then file 20, which holds key 19 and is referred to by the SST holding keys 5-20
  // then file 21, which holds key 20 and is not the earliest key in any SST
  // then files 22-101, holding keys 21-100

}

TEST_F(DBVLogTest, ActiveRecycleTriggerTest1) {
  SetUpForActiveRecycleTest();

  // For some reason mutable_cf_options_ is not initialized at DestroyAndReopen

  // Verify that we don't pick a compaction when the database is too small
  mutable_cf_options_.fragmentation_active_recycling_trigger = std::vector<int32_t>({10});
  mutable_cf_options_.active_recycling_size_trigger = std::vector<int64_t>({1LL<<30});
  mutable_cf_options_.active_recycling_sst_minct = std::vector<int32_t>({5});
  mutable_cf_options_.active_recycling_sst_maxct = std::vector<int32_t>({15});
  mutable_cf_options_.active_recycling_vlogfile_freed_min = std::vector<int32_t>({7});

  std::unique_ptr<Compaction> compaction(level_compaction_picker.PickCompaction(
      cf_name_, mutable_cf_options_, vstorage_.get(), nullptr));
  ASSERT_TRUE(compaction.get() == nullptr);

  // Verify that we don't pick a compaction with a 20% size requirement
  mutable_cf_options_.fragmentation_active_recycling_trigger = std::vector<int32_t>({20});
  mutable_cf_options_.active_recycling_size_trigger = std::vector<int64_t>({0});
  mutable_cf_options_.active_recycling_sst_minct = std::vector<int32_t>({5});
  mutable_cf_options_.active_recycling_sst_maxct = std::vector<int32_t>({15});
  mutable_cf_options_.active_recycling_vlogfile_freed_min = std::vector<int32_t>({7});

  compaction.reset(level_compaction_picker.PickCompaction(
      cf_name_, mutable_cf_options_, vstorage_.get(), nullptr));
  ASSERT_TRUE(compaction.get() == nullptr);

  // verify that we do pick a compaction at a 10% size requirement
  mutable_cf_options_.fragmentation_active_recycling_trigger = std::vector<int32_t>({10});
  mutable_cf_options_.active_recycling_size_trigger = std::vector<int64_t>({0});
  mutable_cf_options_.active_recycling_sst_minct = std::vector<int32_t>({5});
  mutable_cf_options_.active_recycling_sst_maxct = std::vector<int32_t>({15});
  mutable_cf_options_.active_recycling_vlogfile_freed_min = std::vector<int32_t>({7});

  compaction.reset(level_compaction_picker.PickCompaction(
      cf_name_, mutable_cf_options_, vstorage_.get(), nullptr));
  ASSERT_FALSE(compaction.get() == nullptr);
  ASSERT_EQ(15, compaction->inputs()->size());  // get the max 15 SSTs allowed
  ASSERT_EQ(30, compaction->lastfileno());   // pick up the VLog files for them, plus the 15 orphaned VLog files

  // unfortunately, once we pick a compaction the files in it are marked as running the compaction and are not unmarked even if we
  // delete the compaction.  So once we get a compaction we have to move on to the next test
  compaction.reset();
}

TEST_F(DBVLogTest, ActiveRecycleTriggerTest2) {
  SetUpForActiveRecycleTest();

  // testing minct.  Set vlogfile_freed_min to 0, then verify responsiveness to min.  freed_min=0 is a debug mode that doesn't look for SSTs past the minimum
  mutable_cf_options_.fragmentation_active_recycling_trigger = std::vector<int32_t>({10});
  mutable_cf_options_.active_recycling_size_trigger = std::vector<int64_t>({0});
  mutable_cf_options_.active_recycling_sst_minct = std::vector<int32_t>({3});
  mutable_cf_options_.active_recycling_sst_maxct = std::vector<int32_t>({5});
  mutable_cf_options_.active_recycling_vlogfile_freed_min = std::vector<int32_t>({0});

  std::unique_ptr<Compaction> compaction(level_compaction_picker.PickCompaction(
      cf_name_, mutable_cf_options_, vstorage_.get(), nullptr));
  ASSERT_FALSE(compaction.get() == nullptr);
  ASSERT_EQ(3, compaction->inputs()->size());
  ASSERT_EQ(3, compaction->lastfileno());

  // unfortunately, once we pick a compaction the files in it are marked as running the compaction and are not unmarked even if we
  // delete the compaction.  So once we get a compaction we have to move on to the next test
  compaction.reset();
}


TEST_F(DBVLogTest, ActiveRecycleTriggerTest3) {
  SetUpForActiveRecycleTest();

  // testing minct.  Set vlogfile_freed_min to 0, then verify responsiveness to min.  freed_min=0 is a debug mode that doesn't look for SSTs past the minimum
  mutable_cf_options_.fragmentation_active_recycling_trigger = std::vector<int32_t>({10});
  mutable_cf_options_.active_recycling_size_trigger = std::vector<int64_t>({0});
  mutable_cf_options_.active_recycling_sst_minct = std::vector<int32_t>({4});
  mutable_cf_options_.active_recycling_sst_maxct = std::vector<int32_t>({5});
  mutable_cf_options_.active_recycling_vlogfile_freed_min = std::vector<int32_t>({0});

  std::unique_ptr<Compaction> compaction(level_compaction_picker.PickCompaction(
      cf_name_, mutable_cf_options_, vstorage_.get(), nullptr));
  ASSERT_FALSE(compaction.get() == nullptr);
  ASSERT_EQ(4, compaction->inputs()->size());
  ASSERT_EQ(4, compaction->lastfileno());

  // unfortunately, once we pick a compaction the files in it are marked as running the compaction and are not unmarked even if we
  // delete the compaction.  So once we get a compaction we have to move on to the next test
  compaction.reset();
}

TEST_F(DBVLogTest, ActiveRecycleTriggerTest4) {
  SetUpForActiveRecycleTest();

  // testing maxct.  Move vlogfile_freed_min off of 0, then verify responsiveness to maxct.
  mutable_cf_options_.fragmentation_active_recycling_trigger = std::vector<int32_t>({10});
  mutable_cf_options_.active_recycling_size_trigger = std::vector<int64_t>({0});
  mutable_cf_options_.active_recycling_sst_minct = std::vector<int32_t>({3});
  mutable_cf_options_.active_recycling_sst_maxct = std::vector<int32_t>({4});
  mutable_cf_options_.active_recycling_vlogfile_freed_min = std::vector<int32_t>({1});

  std::unique_ptr<Compaction> compaction(level_compaction_picker.PickCompaction(
      cf_name_, mutable_cf_options_, vstorage_.get(), nullptr));
  ASSERT_FALSE(compaction.get() == nullptr);
  ASSERT_EQ(4, compaction->inputs()->size());
  ASSERT_EQ(4, compaction->lastfileno());

  // unfortunately, once we pick a compaction the files in it are marked as running the compaction and are not unmarked even if we
  // delete the compaction.  So once we get a compaction we have to move on to the next test
  compaction.reset();
}

TEST_F(DBVLogTest, ActiveRecycleTriggerTest5) {
  SetUpForActiveRecycleTest();

  // testing maxct.  Move vlogfile_freed_min off of 0, then verify responsiveness to maxct.
  mutable_cf_options_.fragmentation_active_recycling_trigger = std::vector<int32_t>({10});
  mutable_cf_options_.active_recycling_size_trigger = std::vector<int64_t>({0});
  mutable_cf_options_.active_recycling_sst_minct = std::vector<int32_t>({3});
  mutable_cf_options_.active_recycling_sst_maxct = std::vector<int32_t>({5});
  mutable_cf_options_.active_recycling_vlogfile_freed_min = std::vector<int32_t>({1});

  std::unique_ptr<Compaction> compaction(level_compaction_picker.PickCompaction(
      cf_name_, mutable_cf_options_, vstorage_.get(), nullptr));
  ASSERT_FALSE(compaction.get() == nullptr);
  ASSERT_EQ(5, compaction->inputs()->size());
  ASSERT_EQ(5, compaction->lastfileno());

  // unfortunately, once we pick a compaction the files in it are marked as running the compaction and are not unmarked even if we
  // delete the compaction.  So once we get a compaction we have to move on to the next test
  compaction.reset();
}

TEST_F(DBVLogTest, ActiveRecycleTriggerTest6) {
  SetUpForActiveRecycleTest();

  // testing freed_min.  set sst_minct to 0 (debugging mode), then verify responsiveness to freed_min.
  mutable_cf_options_.fragmentation_active_recycling_trigger = std::vector<int32_t>({10});
  mutable_cf_options_.active_recycling_size_trigger = std::vector<int64_t>({0});
  mutable_cf_options_.active_recycling_sst_minct = std::vector<int32_t>({0});
  mutable_cf_options_.active_recycling_sst_maxct = std::vector<int32_t>({5});
  mutable_cf_options_.active_recycling_vlogfile_freed_min = std::vector<int32_t>({2});

  std::unique_ptr<Compaction> compaction(level_compaction_picker.PickCompaction(
      cf_name_, mutable_cf_options_, vstorage_.get(), nullptr));
  ASSERT_FALSE(compaction.get() == nullptr);
  ASSERT_EQ(2, compaction->inputs()->size());
  ASSERT_EQ(2, compaction->lastfileno());

  // unfortunately, once we pick a compaction the files in it are marked as running the compaction and are not unmarked even if we
  // delete the compaction.  So once we get a compaction we have to move on to the next test
  compaction.reset();
}

TEST_F(DBVLogTest, ActiveRecycleTriggerTest7) {
  SetUpForActiveRecycleTest();

  // testing freed_min.  set sst_minct to 0 (debugging mode), then verify responsiveness to freed_min.
  mutable_cf_options_.fragmentation_active_recycling_trigger = std::vector<int32_t>({10});
  mutable_cf_options_.active_recycling_size_trigger = std::vector<int64_t>({0});
  mutable_cf_options_.active_recycling_sst_minct = std::vector<int32_t>({0});
  mutable_cf_options_.active_recycling_sst_maxct = std::vector<int32_t>({5});
  mutable_cf_options_.active_recycling_vlogfile_freed_min = std::vector<int32_t>({3});

  std::unique_ptr<Compaction> compaction(level_compaction_picker.PickCompaction(
      cf_name_, mutable_cf_options_, vstorage_.get(), nullptr));
  ASSERT_FALSE(compaction.get() == nullptr);
  ASSERT_EQ(3, compaction->inputs()->size());
  ASSERT_EQ(3, compaction->lastfileno());

  // unfortunately, once we pick a compaction the files in it are marked as running the compaction and are not unmarked even if we
  // delete the compaction.  So once we get a compaction we have to move on to the next test
  compaction.reset();
}


#if 0


TEST_F(DBVLogTest, VLogRefsTest) {
  Options options = CurrentOptions();
  options.write_buffer_size = 100 * 1024 * 1024;  // 100MB write buffer: hold all keys
  options.num_levels = 4;
  options.level0_file_num_compaction_trigger = 3;
  options.level0_slowdown_writes_trigger = 5;
  options.level0_stop_writes_trigger = 11;
  options.max_background_compactions = 3;
  options.max_bytes_for_level_base = 100 * (1LL<<20);  // keep level1 big too
  options.max_bytes_for_level_multiplier = 10;

  const int32_t key_size = 16384;
  const int32_t value_size = 16;  // k+v=16KB
  const int32_t max_keys_per_file = 3;  // number of keys we want in each file
  options.target_file_size_base = max_keys_per_file * (key_size+value_size);  // file size limit, to put 3 refs per file (first, middle, last)

  options.vlogring_activation_level = std::vector<int32_t>({0});
  options.min_indirect_val_size = std::vector<size_t>({0});
  options.fraction_remapped_during_compaction = std::vector<int32_t>({50});
  options.fraction_remapped_during_active_recycling = std::vector<int32_t>({25});
  options.fragmentation_active_recycling_trigger = std::vector<int32_t>({25});
  options.fragmentation_active_recycling_klaxon = std::vector<int32_t>({50});
  options.active_recycling_sst_minct = std::vector<int32_t>({5});
  options.active_recycling_sst_maxct = std::vector<int32_t>({15});
  options.active_recycling_vlogfile_freed_min = std::vector<int32_t>({7});
  options.compaction_picker_age_importance = std::vector<int32_t>({100});
  options.ring_compression_style = std::vector<CompressionType>({kNoCompression});
  options.vlogfile_max_size = std::vector<uint64_t>({4LL << 20});  // 4MB

  Random rnd(301);

  // Create SSTs, with 1 key per file.  The key for files 1 and 6 are as long as 3 normal keys
  // flush each file to L0, then compact into L1 to produce 1 VLog file per SST

  // compact all the files into L2
  // this will produce 1 SST per 3 keys (1 key, for SSTs 1 and 6).

  // pick a compaction of everything, to get references to the files
  // go through the earliest-references, verifying that they match the expected

  // repeat total of 6 times, permuting the keys in each block of 3.  Results should not change

  // repeat again, making the very last key a single-key file.  Results should not change

  int64_t keys_per_file[] = {max_keys_per_file,1,max_keys_per_file,max_keys_per_file,max_keys_per_file,1,max_keys_per_file};
  const int32_t num_files = sizeof(keys_per_file)/sizeof(keys_per_file[0]);
 
  // loop twice, once with the size of the last file set to 1
  for(int finalkey=0;finalkey<2;finalkey++){
    // loop over each permutation of inputs
    for(int perm=0;perm<6;++perm){

      // Create SSTs, with 1 key per file.  Flush each file to L0, then compact into L1 to produce 1 VLog file per SST

      // Verify # VLog files created

      // compact all the files into L2
      // Verify correct # SSTs

      // pick a compaction of everything, to get references to the files
      // go through the earliest-references, verifying that they match the expected

    }

    keys_per_file[num_files-1] = 1;  // make the last file 1 key long for the second run
  }


  int32_t mappct=20;
  double mapfrac = mappct * 0.01;
printf("%f\n",mapfrac);  // scaf speedup break;
  // turn off remapping
  options.fraction_remapped_during_compaction = std::vector<int32_t>({mappct});
  options.fragmentation_active_recycling_trigger = std::vector<int32_t>({10});
  options.active_recycling_size_trigger = std::vector<int64_t>({0});

  DestroyAndReopen(options);
  // write 100 files in key order, flushing each to give it a VLog file
  std::string val0string;
  for(int key = 0;key<nkeys;++key){
    std::string keystring = Key(key);
    Slice keyslice = keystring;  // the key we will add
    std::string valstring = RandomString(&rnd, value_size);
    if(key==0)val0string = valstring;
    Slice valslice = valstring;
    ASSERT_OK(Put(keyslice, valslice));
    ASSERT_EQ(valslice, Get(Key(key)));
    // Flush into L0
    ASSERT_OK(Flush());
    // Compact into L1, which will write the VLog files.  At this point there is nothing to remap
    CompactRangeOptions compact_options;
    compact_options.change_level = true;
    compact_options.target_level = 1;
    ASSERT_OK(db_->CompactRange(compact_options, &keyslice, &keyslice));
  }
  // Verify 100 SSTs and 100 VLog files
  ASSERT_EQ("0,100", FilesPerLevel(0));
  std::vector<uint64_t> vlogfilesizes;  // sizes of all the VLog files
  ListVLogFileSizes(this,vlogfilesizes);
  ASSERT_EQ(100, vlogfilesizes.size());
  // Verify total file size is pretty close to right
  int64_t totalsize=0;  // place to build file stats
  for(size_t i=0;i<vlogfilesizes.size();++i){
   totalsize += vlogfilesizes[i];
  }
  ASSERT_GT(1000, std::abs(totalsize-(value_size+5)*nkeys));
  // compact between 5% and 20%.  This should remap all, leaving 15% fragmentation.
  CompactRangeOptions remap_options;
  remap_options.change_level = true;
  remap_options.target_level = 1;
  remap_options.bottommost_level_compaction = BottommostLevelCompaction::kForce;
  remap_options.exclusive_manual_compaction = false;
  ASSERT_OK(db_->CompactRange(remap_options, &Slice(Key((int)(nkeys*0.05))), &Slice(Key((int)(nkeys*0.20)))));
  // The user compaction and the AR compaction should both run.
  printf("%s\n", FilesPerLevel(0).c_str());  // scaf

  NewVersionStorage(6, kCompactionStyleLevel);
  UpdateVersionStorageInfo();

  // Now we have 5 VLog files, numbered 1-5, holding keys 0-4, referred to by SSTs 0-4
  // then files 6-19, which have no reference
  // then file 20, which holds key 19 and is referred to by the SST holding keys 5-20
  // then file 21, which holds key 20 and is not the earliest key in any SST
  // then files 22-101, holding keys 21-100

}


TEST_F(DBVLogTest, SkipStatsUpdateTest) {
  // This test verify UpdateAccumulatedStats is not on
  // if options.skip_stats_update_on_db_open = true
  // The test will need to be updated if the internal behavior changes.

  Options options = DeletionTriggerOptions(CurrentOptions());
  options.env = env_;
  DestroyAndReopen(options);
  Random rnd(301);

  const int kTestSize = kCDTKeysPerBuffer * 512;
  std::vector<std::string> values;
  for (int k = 0; k < kTestSize; ++k) {
    values.push_back(RandomString(&rnd, kCDTValueSize));
    ASSERT_OK(Put(Key(k), values[k]));
  }
  dbfull()->TEST_WaitForFlushMemTable();
  dbfull()->TEST_WaitForCompact();

  // Reopen the DB with stats-update disabled
  options.skip_stats_update_on_db_open = true;
  env_->random_file_open_counter_.store(0);
  Reopen(options);

  // As stats-update is disabled, we expect a very low number of
  // random file open.
  // Note that this number must be changed accordingly if we change
  // the number of files needed to be opened in the DB::Open process.
#ifdef INDIRECT_VALUE_SUPPORT
  const int kMaxFileOpenCount = 50;  // more files when there is Value Logging
#else
  const int kMaxFileOpenCount = 10;
#endif
  ASSERT_LT(env_->random_file_open_counter_.load(), kMaxFileOpenCount);

  // Repeat the reopen process, but this time we enable
  // stats-update.
  options.skip_stats_update_on_db_open = false;
  env_->random_file_open_counter_.store(0);
  Reopen(options);

  // Since we do a normal stats update on db-open, there
  // will be more random open files.
  ASSERT_GT(env_->random_file_open_counter_.load(), kMaxFileOpenCount);
}

TEST_F(DBVLogTest, TestTableReaderForCompaction) {
  Options options = CurrentOptions();
  options.env = env_;
  options.new_table_reader_for_compaction_inputs = true;
  options.max_open_files = 100;
  options.level0_file_num_compaction_trigger = 3;
  DestroyAndReopen(options);
  Random rnd(301);

  int num_table_cache_lookup = 0;
  int num_new_table_reader = 0;
  rocksdb::SyncPoint::GetInstance()->SetCallBack(
      "TableCache::FindTable:0", [&](void* arg) {
        assert(arg != nullptr);
        bool no_io = *(reinterpret_cast<bool*>(arg));
        if (!no_io) {
          // filter out cases for table properties queries.
          num_table_cache_lookup++;
        }
      });
  rocksdb::SyncPoint::GetInstance()->SetCallBack(
      "TableCache::GetTableReader:0",
      [&](void* arg) { num_new_table_reader++; });
  rocksdb::SyncPoint::GetInstance()->EnableProcessing();

  for (int k = 0; k < options.level0_file_num_compaction_trigger; ++k) {
    ASSERT_OK(Put(Key(k), Key(k)));
    ASSERT_OK(Put(Key(10 - k), "bar"));
    if (k < options.level0_file_num_compaction_trigger - 1) {
      num_table_cache_lookup = 0;
      Flush();
      dbfull()->TEST_WaitForCompact();
      // preloading iterator issues one table cache lookup and create
      // a new table reader.
      ASSERT_EQ(num_table_cache_lookup, 1);
      ASSERT_EQ(num_new_table_reader, 1);

      num_table_cache_lookup = 0;
      num_new_table_reader = 0;
      ASSERT_EQ(Key(k), Get(Key(k)));
      // lookup iterator from table cache and no need to create a new one.
      ASSERT_EQ(num_table_cache_lookup, 1);
      ASSERT_EQ(num_new_table_reader, 0);
    }
  }

  num_table_cache_lookup = 0;
  num_new_table_reader = 0;
  Flush();
  dbfull()->TEST_WaitForCompact();
  // Preloading iterator issues one table cache lookup and creates
  // a new table reader. One file is created for flush and one for compaction.
  // Compaction inputs make no table cache look-up for data/range deletion
  // iterators
  ASSERT_EQ(num_table_cache_lookup, 2);
  // Create new iterator for:
  // (1) 1 for verifying flush results
  // (2) 3 for compaction input files
  // (3) 1 for verifying compaction results.
  ASSERT_EQ(num_new_table_reader, 5);

  num_table_cache_lookup = 0;
  num_new_table_reader = 0;
  ASSERT_EQ(Key(1), Get(Key(1)));
  ASSERT_EQ(num_table_cache_lookup, 1);
  ASSERT_EQ(num_new_table_reader, 0);

  num_table_cache_lookup = 0;
  num_new_table_reader = 0;
  CompactRangeOptions cro;
  cro.change_level = true;
  cro.target_level = 2;
  cro.bottommost_level_compaction = BottommostLevelCompaction::kForce;
  db_->CompactRange(cro, nullptr, nullptr);
  // Only verifying compaction outputs issues one table cache lookup
  // for both data block and range deletion block).
  ASSERT_EQ(num_table_cache_lookup, 1);
  // One for compaction input, one for verifying compaction results.
  ASSERT_EQ(num_new_table_reader, 2);

  num_table_cache_lookup = 0;
  num_new_table_reader = 0;
  ASSERT_EQ(Key(1), Get(Key(1)));
  ASSERT_EQ(num_table_cache_lookup, 1);
  ASSERT_EQ(num_new_table_reader, 0);

  rocksdb::SyncPoint::GetInstance()->ClearAllCallBacks();
}

TEST_P(DBVLogTestWithParam, CompactionDeletionTriggerReopen) {
  for (int tid = 0; tid < 2; ++tid) {
    uint64_t db_size[3];
    Options options = DeletionTriggerOptions(CurrentOptions());
    options.max_subcompactions = max_subcompactions_;

    if (tid == 1) {
      // second pass with universal compaction
      options.compaction_style = kCompactionStyleUniversal;
      options.num_levels = 1;
    }

    DestroyAndReopen(options);
    Random rnd(301);

    // round 1 --- insert key/value pairs.
    const int kTestSize = kCDTKeysPerBuffer * 512;
    std::vector<std::string> values;
    for (int k = 0; k < kTestSize; ++k) {
      values.push_back(RandomString(&rnd, kCDTValueSize));
      ASSERT_OK(Put(Key(k), values[k]));
    }
    dbfull()->TEST_WaitForFlushMemTable();
    dbfull()->TEST_WaitForCompact();
    db_size[0] = Size(Key(0), Key(kTestSize - 1));
    Close();

    // round 2 --- disable auto-compactions and issue deletions.
    options.create_if_missing = false;
    options.disable_auto_compactions = true;
    Reopen(options);

    for (int k = 0; k < kTestSize; ++k) {
      ASSERT_OK(Delete(Key(k)));
    }
    db_size[1] = Size(Key(0), Key(kTestSize - 1));
    Close();
    // as auto_compaction is off, we shouldn't see too much reduce
    // in db size.
    ASSERT_LT(db_size[0] / 3, db_size[1]);

    // round 3 --- reopen db with auto_compaction on and see if
    // deletion compensation still work.
    options.disable_auto_compactions = false;
    Reopen(options);
    // insert relatively small amount of data to trigger auto compaction.
    for (int k = 0; k < kTestSize / 10; ++k) {
      ASSERT_OK(Put(Key(k), values[k]));
    }
    dbfull()->TEST_WaitForFlushMemTable();
    dbfull()->TEST_WaitForCompact();
    db_size[2] = Size(Key(0), Key(kTestSize - 1));
    // this time we're expecting significant drop in size.
    ASSERT_GT(db_size[0] / 3, db_size[2]);
  }
}

TEST_F(DBVLogTest, DisableStatsUpdateReopen) {
  uint64_t db_size[3];
  for (int test = 0; test < 2; ++test) {
    Options options = DeletionTriggerOptions(CurrentOptions());
    options.skip_stats_update_on_db_open = (test == 0);

    env_->random_read_counter_.Reset();
    DestroyAndReopen(options);
    Random rnd(301);

    // round 1 --- insert key/value pairs.
    const int kTestSize = kCDTKeysPerBuffer * 512;
    std::vector<std::string> values;
    for (int k = 0; k < kTestSize; ++k) {
      values.push_back(RandomString(&rnd, kCDTValueSize));
      ASSERT_OK(Put(Key(k), values[k]));
    }
    dbfull()->TEST_WaitForFlushMemTable();
    dbfull()->TEST_WaitForCompact();
    db_size[0] = Size(Key(0), Key(kTestSize - 1));
    Close();

    // round 2 --- disable auto-compactions and issue deletions.
    options.create_if_missing = false;
    options.disable_auto_compactions = true;

    env_->random_read_counter_.Reset();
    Reopen(options);

    for (int k = 0; k < kTestSize; ++k) {
      ASSERT_OK(Delete(Key(k)));
    }
    db_size[1] = Size(Key(0), Key(kTestSize - 1));
    Close();
    // as auto_compaction is off, we shouldn't see too much reduce
    // in db size.
    ASSERT_LT(db_size[0] / 3, db_size[1]);

    // round 3 --- reopen db with auto_compaction on and see if
    // deletion compensation still work.
    options.disable_auto_compactions = false;
    Reopen(options);
    dbfull()->TEST_WaitForFlushMemTable();
    dbfull()->TEST_WaitForCompact();
    db_size[2] = Size(Key(0), Key(kTestSize - 1));

    if (options.skip_stats_update_on_db_open) {
      // If update stats on DB::Open is disable, we don't expect
      // deletion entries taking effect.
      ASSERT_LT(db_size[0] / 3, db_size[2]);
    } else {
      // Otherwise, we should see a significant drop in db size.
      ASSERT_GT(db_size[0] / 3, db_size[2]);
    }
  }
}


TEST_P(DBVLogTestWithParam, CompactionTrigger) {
  const int kNumKeysPerFile = 100;

  Options options = CurrentOptions();
  options.write_buffer_size = 110 << 10;  // 110KB
  options.arena_block_size = 4 << 10;
  options.num_levels = 3;
  options.level0_file_num_compaction_trigger = 3;
  options.max_subcompactions = max_subcompactions_;
  options.memtable_factory.reset(new SpecialSkipListFactory(kNumKeysPerFile));
  CreateAndReopenWithCF({"pikachu"}, options);

  Random rnd(301);

  for (int num = 0; num < options.level0_file_num_compaction_trigger - 1;
       num++) {
    std::vector<std::string> values;
    // Write 100KB (100 values, each 1K)
    for (int i = 0; i < kNumKeysPerFile; i++) {
      values.push_back(RandomString(&rnd, 990));
      ASSERT_OK(Put(1, Key(i), values[i]));
    }
    // put extra key to trigger flush
    ASSERT_OK(Put(1, "", ""));
    dbfull()->TEST_WaitForFlushMemTable(handles_[1]);
    ASSERT_EQ(NumTableFilesAtLevel(0, 1), num + 1);
  }

  // generate one more file in level-0, and should trigger level-0 compaction
  std::vector<std::string> values;
  for (int i = 0; i < kNumKeysPerFile; i++) {
    values.push_back(RandomString(&rnd, 990));
    ASSERT_OK(Put(1, Key(i), values[i]));
  }
  // put extra key to trigger flush
  ASSERT_OK(Put(1, "", ""));
  dbfull()->TEST_WaitForCompact();

  ASSERT_EQ(NumTableFilesAtLevel(0, 1), 0);
  ASSERT_EQ(NumTableFilesAtLevel(1, 1), 1);
}

TEST_F(DBVLogTest, BGCompactionsAllowed) {
  // Create several column families. Make compaction triggers in all of them
  // and see number of compactions scheduled to be less than allowed.
  const int kNumKeysPerFile = 100;

  Options options = CurrentOptions();
  options.write_buffer_size = 110 << 10;  // 110KB
  options.arena_block_size = 4 << 10;
  options.num_levels = 3;
  // Should speed up compaction when there are 4 files.
  options.level0_file_num_compaction_trigger = 2;
  options.level0_slowdown_writes_trigger = 20;
  options.soft_pending_compaction_bytes_limit = 1 << 30;  // Infinitely large
  options.max_background_compactions = 3;
  options.memtable_factory.reset(new SpecialSkipListFactory(kNumKeysPerFile));

  // Block all threads in thread pool.
  const size_t kTotalTasks = 4;
  env_->SetBackgroundThreads(4, Env::LOW);
  test::SleepingBackgroundTask sleeping_tasks[kTotalTasks];
  for (size_t i = 0; i < kTotalTasks; i++) {
    env_->Schedule(&test::SleepingBackgroundTask::DoSleepTask,
                   &sleeping_tasks[i], Env::Priority::LOW);
    sleeping_tasks[i].WaitUntilSleeping();
  }

  CreateAndReopenWithCF({"one", "two", "three"}, options);

  Random rnd(301);
  for (int cf = 0; cf < 4; cf++) {
    for (int num = 0; num < options.level0_file_num_compaction_trigger; num++) {
      for (int i = 0; i < kNumKeysPerFile; i++) {
        ASSERT_OK(Put(cf, Key(i), ""));
      }
      // put extra key to trigger flush
      ASSERT_OK(Put(cf, "", ""));
      dbfull()->TEST_WaitForFlushMemTable(handles_[cf]);
      ASSERT_EQ(NumTableFilesAtLevel(0, cf), num + 1);
    }
  }

  // Now all column families qualify compaction but only one should be
  // scheduled, because no column family hits speed up condition.
  ASSERT_EQ(1, env_->GetThreadPoolQueueLen(Env::Priority::LOW));

  // Create two more files for one column family, which triggers speed up
  // condition, three compactions will be scheduled.
  for (int num = 0; num < options.level0_file_num_compaction_trigger; num++) {
    for (int i = 0; i < kNumKeysPerFile; i++) {
      ASSERT_OK(Put(2, Key(i), ""));
    }
    // put extra key to trigger flush
    ASSERT_OK(Put(2, "", ""));
    dbfull()->TEST_WaitForFlushMemTable(handles_[2]);
    ASSERT_EQ(options.level0_file_num_compaction_trigger + num + 1,
              NumTableFilesAtLevel(0, 2));
  }
  ASSERT_EQ(3, env_->GetThreadPoolQueueLen(Env::Priority::LOW));

  // Unblock all threads to unblock all compactions.
  for (size_t i = 0; i < kTotalTasks; i++) {
    sleeping_tasks[i].WakeUp();
    sleeping_tasks[i].WaitUntilDone();
  }
  dbfull()->TEST_WaitForCompact();

  // Verify number of compactions allowed will come back to 1.

  for (size_t i = 0; i < kTotalTasks; i++) {
    sleeping_tasks[i].Reset();
    env_->Schedule(&test::SleepingBackgroundTask::DoSleepTask,
                   &sleeping_tasks[i], Env::Priority::LOW);
    sleeping_tasks[i].WaitUntilSleeping();
  }
  for (int cf = 0; cf < 4; cf++) {
    for (int num = 0; num < options.level0_file_num_compaction_trigger; num++) {
      for (int i = 0; i < kNumKeysPerFile; i++) {
        ASSERT_OK(Put(cf, Key(i), ""));
      }
      // put extra key to trigger flush
      ASSERT_OK(Put(cf, "", ""));
      dbfull()->TEST_WaitForFlushMemTable(handles_[cf]);
      ASSERT_EQ(NumTableFilesAtLevel(0, cf), num + 1);
    }
  }

  // Now all column families qualify compaction but only one should be
  // scheduled, because no column family hits speed up condition.
  ASSERT_EQ(1, env_->GetThreadPoolQueueLen(Env::Priority::LOW));

  for (size_t i = 0; i < kTotalTasks; i++) {
    sleeping_tasks[i].WakeUp();
    sleeping_tasks[i].WaitUntilDone();
  }
}

TEST_P(DBVLogTestWithParam, CompactionsGenerateMultipleFiles) {
  Options options = CurrentOptions();
  options.write_buffer_size = 100000000;        // Large write buffer
  options.max_subcompactions = max_subcompactions_;
  CreateAndReopenWithCF({"pikachu"}, options);
  const int value_size=100000;

  Random rnd(301);

  // Write 8MB (80 values, each 100K)
  ASSERT_EQ(NumTableFilesAtLevel(0, 1), 0);
  std::vector<std::string> values;
  for (int i = 0; i < 80; i++) {
    values.push_back(RandomString(&rnd, value_size));
    ASSERT_OK(PutBig(1, i, value_size, values[i]));
  }

  // Reopening moves updates to level-0
  ReopenWithColumnFamilies({"default", "pikachu"}, options);
  dbfull()->TEST_CompactRange(0, nullptr, nullptr, handles_[1],
                              true /* disallow trivial move */);

  ASSERT_EQ(NumTableFilesAtLevel(0, 1), 0);
  ASSERT_GT(NumTableFilesAtLevel(1, 1), 1);
  for (int i = 0; i < 80; i++) {
    ASSERT_EQ(Get(1, KeyBig(i,value_size)), ValueBig(values[i]));
  }
}

TEST_F(DBVLogTest, MinorCompactionsHappen) {
  do {
    Options options = CurrentOptions();
    options.write_buffer_size = 10000;
    CreateAndReopenWithCF({"pikachu"}, options);

    const int N = 500;

    int starting_num_tables = TotalTableFiles(1);
    for (int i = 0; i < N; i++) {
      ASSERT_OK(Put(1, Key(i), Key(i) + std::string(1000, 'v')));
    }
    int ending_num_tables = TotalTableFiles(1);
    ASSERT_GT(ending_num_tables, starting_num_tables);

    for (int i = 0; i < N; i++) {
      ASSERT_EQ(Key(i) + std::string(1000, 'v'), Get(1, Key(i)));
    }

    ReopenWithColumnFamilies({"default", "pikachu"}, options);

    for (int i = 0; i < N; i++) {
      ASSERT_EQ(Key(i) + std::string(1000, 'v'), Get(1, Key(i)));
    }
  } while (ChangeCompactOptions());
}

TEST_F(DBVLogTest, UserKeyCrossFile1) {
  Options options = CurrentOptions();
  options.compaction_style = kCompactionStyleLevel;
  options.level0_file_num_compaction_trigger = 3;

  DestroyAndReopen(options);

  // create first file and flush to l0
  Put("4", "A");
  Put("3", "A");
  Flush();
  dbfull()->TEST_WaitForFlushMemTable();

  Put("2", "A");
  Delete("3");
  Flush();
  dbfull()->TEST_WaitForFlushMemTable();
  ASSERT_EQ("NOT_FOUND", Get("3"));

  // move both files down to l1
  dbfull()->CompactRange(CompactRangeOptions(), nullptr, nullptr);
  ASSERT_EQ("NOT_FOUND", Get("3"));

  for (int i = 0; i < 3; i++) {
    Put("2", "B");
    Flush();
    dbfull()->TEST_WaitForFlushMemTable();
  }
  dbfull()->TEST_WaitForCompact();

  ASSERT_EQ("NOT_FOUND", Get("3"));
}

TEST_F(DBVLogTest, UserKeyCrossFile2) {
  Options options = CurrentOptions();
  options.compaction_style = kCompactionStyleLevel;
  options.level0_file_num_compaction_trigger = 3;

  DestroyAndReopen(options);

  // create first file and flush to l0
  Put("4", "A");
  Put("3", "A");
  Flush();
  dbfull()->TEST_WaitForFlushMemTable();

  Put("2", "A");
  SingleDelete("3");
  Flush();
  dbfull()->TEST_WaitForFlushMemTable();
  ASSERT_EQ("NOT_FOUND", Get("3"));

  // move both files down to l1
  dbfull()->CompactRange(CompactRangeOptions(), nullptr, nullptr);
  ASSERT_EQ("NOT_FOUND", Get("3"));

  for (int i = 0; i < 3; i++) {
    Put("2", "B");
    Flush();
    dbfull()->TEST_WaitForFlushMemTable();
  }
  dbfull()->TEST_WaitForCompact();

  ASSERT_EQ("NOT_FOUND", Get("3"));
}

TEST_F(DBVLogTest, ZeroSeqIdCompaction) {
  Options options = CurrentOptions();
  options.compaction_style = kCompactionStyleLevel;
  options.level0_file_num_compaction_trigger = 3;

  FlushedFileCollector* collector = new FlushedFileCollector();
  options.listeners.emplace_back(collector);

  // compaction options
  CompactionOptions compact_opt;
  compact_opt.compression = kNoCompression;
  compact_opt.output_file_size_limit = 4096;
  const size_t key_len =
    static_cast<size_t>(compact_opt.output_file_size_limit) / 5;

  DestroyAndReopen(options);

  std::vector<const Snapshot*> snaps;

  // create first file and flush to l0
  for (auto& key : {"1", "2", "3", "3", "3", "3"}) {
    Put(key, std::string(key_len, 'A'));
    snaps.push_back(dbfull()->GetSnapshot());
  }
  Flush();
  dbfull()->TEST_WaitForFlushMemTable();

  // create second file and flush to l0
  for (auto& key : {"3", "4", "5", "6", "7", "8"}) {
    Put(key, std::string(key_len, 'A'));
    snaps.push_back(dbfull()->GetSnapshot());
  }
  Flush();
  dbfull()->TEST_WaitForFlushMemTable();

  // move both files down to l1
  dbfull()->CompactFiles(compact_opt, collector->GetFlushedFiles(), 1);

  // release snap so that first instance of key(3) can have seqId=0
  for (auto snap : snaps) {
    dbfull()->ReleaseSnapshot(snap);
  }

  // create 3 files in l0 so to trigger compaction
  for (int i = 0; i < options.level0_file_num_compaction_trigger; i++) {
    Put("2", std::string(1, 'A'));
    Flush();
    dbfull()->TEST_WaitForFlushMemTable();
  }

  dbfull()->TEST_WaitForCompact();
  ASSERT_OK(Put("", ""));
}

TEST_F(DBVLogTest, ManualCompactionUnknownOutputSize) {
  // github issue #2249
  Options options = CurrentOptions();
  options.compaction_style = kCompactionStyleLevel;
  options.level0_file_num_compaction_trigger = 3;
  DestroyAndReopen(options);

  // create two files in l1 that we can compact
  for (int i = 0; i < 2; ++i) {
    for (int j = 0; j < options.level0_file_num_compaction_trigger; j++) {
      // make l0 files' ranges overlap to avoid trivial move
      Put(std::to_string(2 * i), std::string(1, 'A'));
      Put(std::to_string(2 * i + 1), std::string(1, 'A'));
      Flush();
      dbfull()->TEST_WaitForFlushMemTable();
    }
    dbfull()->TEST_WaitForCompact();
    ASSERT_EQ(NumTableFilesAtLevel(0, 0), 0);
    ASSERT_EQ(NumTableFilesAtLevel(1, 0), i + 1);
  }

  ColumnFamilyMetaData cf_meta;
  dbfull()->GetColumnFamilyMetaData(dbfull()->DefaultColumnFamily(), &cf_meta);
  ASSERT_EQ(2, cf_meta.levels[1].files.size());
  std::vector<std::string> input_filenames;
  for (const auto& sst_file : cf_meta.levels[1].files) {
    input_filenames.push_back(sst_file.name);
  }

  // note CompactionOptions::output_file_size_limit is unset.
  CompactionOptions compact_opt;
  compact_opt.compression = kNoCompression;
  dbfull()->CompactFiles(compact_opt, input_filenames, 1);
}

// Check that writes done during a memtable compaction are recovered
// if the database is shutdown during the memtable compaction.
TEST_F(DBVLogTest, RecoverDuringMemtableCompaction) {
  do {
    Options options = CurrentOptions();
    options.env = env_;
    CreateAndReopenWithCF({"pikachu"}, options);

    // Trigger a long memtable compaction and reopen the database during it
    ASSERT_OK(Put(1, "foo", "v1"));  // Goes to 1st log file
    ASSERT_OK(Put(1, "big1", std::string(10000000, 'x')));  // Fills memtable
    ASSERT_OK(Put(1, "big2", std::string(1000, 'y')));  // Triggers compaction
    ASSERT_OK(Put(1, "bar", "v2"));                     // Goes to new log file

    ReopenWithColumnFamilies({"default", "pikachu"}, options);
    ASSERT_EQ("v1", Get(1, "foo"));
    ASSERT_EQ("v2", Get(1, "bar"));
    ASSERT_EQ(std::string(10000000, 'x'), Get(1, "big1"));
    ASSERT_EQ(std::string(1000, 'y'), Get(1, "big2"));
  } while (ChangeOptions());
}

TEST_P(DBVLogTestWithParam, TrivialMoveOneFile) {
  int32_t trivial_move = 0;
  rocksdb::SyncPoint::GetInstance()->SetCallBack(
      "DBImpl::BackgroundCompaction:TrivialMove",
      [&](void* arg) { trivial_move++; });
  rocksdb::SyncPoint::GetInstance()->EnableProcessing();

  Options options = CurrentOptions();
  options.write_buffer_size = 100000000;
  options.max_subcompactions = max_subcompactions_;
#ifdef INDIRECT_VALUE_SUPPORT
  options.allow_trivial_move = true;
#endif
  DestroyAndReopen(options);

  int32_t num_keys = 80;
  int32_t value_size = 100 * 1024;  // 100 KB

  Random rnd(301);
  std::vector<std::string> values;
  for (int i = 0; i < num_keys; i++) {
    values.push_back(RandomString(&rnd, value_size));
    ASSERT_OK(Put(Key(i), values[i]));
  }

  // Reopening moves updates to L0
  Reopen(options);
  ASSERT_EQ(NumTableFilesAtLevel(0, 0), 1);  // 1 file in L0
  ASSERT_EQ(NumTableFilesAtLevel(1, 0), 0);  // 0 files in L1

  std::vector<LiveFileMetaData> metadata;
  db_->GetLiveFilesMetaData(&metadata);
  ASSERT_EQ(metadata.size(), 1U);
  LiveFileMetaData level0_file = metadata[0];  // L0 file meta

  CompactRangeOptions cro;
  cro.exclusive_manual_compaction = exclusive_manual_compaction_;

  // Compaction will initiate a trivial move from L0 to L1
  dbfull()->CompactRange(cro, nullptr, nullptr);

  // File moved From L0 to L1
  ASSERT_EQ(NumTableFilesAtLevel(0, 0), 0);  // 0 files in L0
  ASSERT_EQ(NumTableFilesAtLevel(1, 0), 1);  // 1 file in L1

  metadata.clear();
  db_->GetLiveFilesMetaData(&metadata);
  ASSERT_EQ(metadata.size(), 1U);
  ASSERT_EQ(metadata[0].name /* level1_file.name */, level0_file.name);
  ASSERT_EQ(metadata[0].size /* level1_file.size */, level0_file.size);

  for (int i = 0; i < num_keys; i++) {
    ASSERT_EQ(Get(Key(i)), values[i]);
  }

  ASSERT_EQ(trivial_move, 1);
  rocksdb::SyncPoint::GetInstance()->DisableProcessing();
}

TEST_P(DBVLogTestWithParam, TrivialMoveNonOverlappingFiles) {
  int32_t trivial_move = 0;
  int32_t non_trivial_move = 0;
  rocksdb::SyncPoint::GetInstance()->SetCallBack(
      "DBImpl::BackgroundCompaction:TrivialMove",
      [&](void* arg) { trivial_move++; });
  rocksdb::SyncPoint::GetInstance()->SetCallBack(
      "DBImpl::BackgroundCompaction:NonTrivial",
      [&](void* arg) { non_trivial_move++; });
  rocksdb::SyncPoint::GetInstance()->EnableProcessing();

  Options options = CurrentOptions();
  options.disable_auto_compactions = true;
  options.write_buffer_size = 10 * 1024 * 1024;
  options.max_subcompactions = max_subcompactions_;
#ifdef INDIRECT_VALUE_SUPPORT
  options.allow_trivial_move = true;
#endif

  DestroyAndReopen(options);
  // non overlapping ranges
  std::vector<std::pair<int32_t, int32_t>> ranges = {
    {100, 199},
    {300, 399},
    {0, 99},
    {200, 299},
    {600, 699},
    {400, 499},
    {500, 550},
    {551, 599},
  };
  int32_t value_size = 10 * 1024;  // 10 KB

  Random rnd(301);
  std::map<int32_t, std::string> values;
  for (size_t i = 0; i < ranges.size(); i++) {
    for (int32_t j = ranges[i].first; j <= ranges[i].second; j++) {
      values[j] = RandomString(&rnd, value_size);
      ASSERT_OK(Put(Key(j), values[j]));
    }
    ASSERT_OK(Flush());
  }

  int32_t level0_files = NumTableFilesAtLevel(0, 0);
  ASSERT_EQ(level0_files, ranges.size());    // Multiple files in L0
  ASSERT_EQ(NumTableFilesAtLevel(1, 0), 0);  // No files in L1

  CompactRangeOptions cro;
  cro.exclusive_manual_compaction = exclusive_manual_compaction_;

  // Since data is non-overlapping we expect compaction to initiate
  // a trivial move
  db_->CompactRange(cro, nullptr, nullptr);
  // We expect that all the files were trivially moved from L0 to L1
  ASSERT_EQ(NumTableFilesAtLevel(0, 0), 0);
  ASSERT_EQ(NumTableFilesAtLevel(1, 0) /* level1_files */, level0_files);

  for (size_t i = 0; i < ranges.size(); i++) {
    for (int32_t j = ranges[i].first; j <= ranges[i].second; j++) {
      ASSERT_EQ(Get(Key(j)), values[j]);
    }
  }

  ASSERT_EQ(trivial_move, 1);
  ASSERT_EQ(non_trivial_move, 0);

  trivial_move = 0;
  non_trivial_move = 0;
  values.clear();
  DestroyAndReopen(options);
  // Same ranges as above but overlapping
  ranges = {
    {100, 199},
    {300, 399},
    {0, 99},
    {200, 299},
    {600, 699},
    {400, 499},
    {500, 560},  // this range overlap with the next one
    {551, 599},
  };
  for (size_t i = 0; i < ranges.size(); i++) {
    for (int32_t j = ranges[i].first; j <= ranges[i].second; j++) {
      values[j] = RandomString(&rnd, value_size);
      ASSERT_OK(Put(Key(j), values[j]));
    }
    ASSERT_OK(Flush());
  }

  db_->CompactRange(cro, nullptr, nullptr);

  for (size_t i = 0; i < ranges.size(); i++) {
    for (int32_t j = ranges[i].first; j <= ranges[i].second; j++) {
      ASSERT_EQ(Get(Key(j)), values[j]);
    }
  }
  ASSERT_EQ(trivial_move, 0);
  ASSERT_EQ(non_trivial_move, 1);

  rocksdb::SyncPoint::GetInstance()->DisableProcessing();
}

TEST_P(DBVLogTestWithParam, TrivialMoveTargetLevel) {
  int32_t trivial_move = 0;
  int32_t non_trivial_move = 0;
  rocksdb::SyncPoint::GetInstance()->SetCallBack(
      "DBImpl::BackgroundCompaction:TrivialMove",
      [&](void* arg) { trivial_move++; });
  rocksdb::SyncPoint::GetInstance()->SetCallBack(
      "DBImpl::BackgroundCompaction:NonTrivial",
      [&](void* arg) { non_trivial_move++; });
  rocksdb::SyncPoint::GetInstance()->EnableProcessing();

  Options options = CurrentOptions();
  options.disable_auto_compactions = true;
  options.write_buffer_size = 10 * 1024 * 1024;
  options.num_levels = 7;
  options.max_subcompactions = max_subcompactions_;
#ifdef INDIRECT_VALUE_SUPPORT
  options.allow_trivial_move = true;
#endif

  DestroyAndReopen(options);
  int32_t value_size = 10 * 1024;  // 10 KB

  // Add 2 non-overlapping files
  Random rnd(301);
  std::map<int32_t, std::string> values;

  // file 1 [0 => 300]
  for (int32_t i = 0; i <= 300; i++) {
    values[i] = RandomString(&rnd, value_size);
    ASSERT_OK(Put(Key(i), values[i]));
  }
  ASSERT_OK(Flush());

  // file 2 [600 => 700]
  for (int32_t i = 600; i <= 700; i++) {
    values[i] = RandomString(&rnd, value_size);
    ASSERT_OK(Put(Key(i), values[i]));
  }
  ASSERT_OK(Flush());

  // 2 files in L0
  ASSERT_EQ("2", FilesPerLevel(0));
  CompactRangeOptions compact_options;
  compact_options.change_level = true;
  compact_options.target_level = 6;
  compact_options.exclusive_manual_compaction = exclusive_manual_compaction_;
  ASSERT_OK(db_->CompactRange(compact_options, nullptr, nullptr));
  // 2 files in L6
  ASSERT_EQ("0,0,0,0,0,0,2", FilesPerLevel(0));

  ASSERT_EQ(trivial_move, 1);
  ASSERT_EQ(non_trivial_move, 0);

  for (int32_t i = 0; i <= 300; i++) {
    ASSERT_EQ(Get(Key(i)), values[i]);
  }
  for (int32_t i = 600; i <= 700; i++) {
    ASSERT_EQ(Get(Key(i)), values[i]);
  }
}

TEST_P(DBVLogTestWithParam, ManualCompactionPartial) {
  int32_t trivial_move = 0;
  int32_t non_trivial_move = 0;
  rocksdb::SyncPoint::GetInstance()->SetCallBack(
      "DBImpl::BackgroundCompaction:TrivialMove",
      [&](void* arg) { trivial_move++; });
  rocksdb::SyncPoint::GetInstance()->SetCallBack(
      "DBImpl::BackgroundCompaction:NonTrivial",
      [&](void* arg) { non_trivial_move++; });
  bool first = true;
  // Purpose of dependencies:
  // 4 -> 1: ensure the order of two non-trivial compactions
  // 5 -> 2 and 5 -> 3: ensure we do a check before two non-trivial compactions
  // are installed
  rocksdb::SyncPoint::GetInstance()->LoadDependency(
      {{"DBCompaction::ManualPartial:4", "DBCompaction::ManualPartial:1"},
       {"DBCompaction::ManualPartial:5", "DBCompaction::ManualPartial:2"},
       {"DBCompaction::ManualPartial:5", "DBCompaction::ManualPartial:3"}});
  rocksdb::SyncPoint::GetInstance()->SetCallBack(
      "DBImpl::BackgroundCompaction:NonTrivial:AfterRun", [&](void* arg) {
        if (first) {
          first = false;
          TEST_SYNC_POINT("DBCompaction::ManualPartial:4");
          TEST_SYNC_POINT("DBCompaction::ManualPartial:3");
        } else {  // second non-trivial compaction
          TEST_SYNC_POINT("DBCompaction::ManualPartial:2");
        }
      });

  rocksdb::SyncPoint::GetInstance()->EnableProcessing();

  Options options = CurrentOptions();
  options.write_buffer_size = 10 * 1024 * 1024;
  options.num_levels = 7;
  options.max_subcompactions = max_subcompactions_;
  options.level0_file_num_compaction_trigger = 3;
  options.max_background_compactions = 3;
  options.target_file_size_base = 1 << 23;  // 8 MB
#ifdef INDIRECT_VALUE_SUPPORT
  options.allow_trivial_move = true;
#endif

  DestroyAndReopen(options);
  int32_t value_size = 10 * 1024;  // 10 KB

  // Add 2 non-overlapping files
  Random rnd(301);
  std::map<int32_t, std::string> values;

  // file 1 [0 => 100]
  for (int32_t i = 0; i < 100; i++) {
    values[i] = RandomString(&rnd, value_size);
    ASSERT_OK(Put(Key(i), values[i]));
  }
  ASSERT_OK(Flush());

  // file 2 [100 => 300]
  for (int32_t i = 100; i < 300; i++) {
    values[i] = RandomString(&rnd, value_size);
    ASSERT_OK(Put(Key(i), values[i]));
  }
  ASSERT_OK(Flush());

  // 2 files in L0
  ASSERT_EQ("2", FilesPerLevel(0));
  CompactRangeOptions compact_options;
  compact_options.change_level = true;
  compact_options.target_level = 6;
  compact_options.exclusive_manual_compaction = exclusive_manual_compaction_;
  // Trivial move the two non-overlapping files to level 6
  ASSERT_OK(db_->CompactRange(compact_options, nullptr, nullptr));
  // 2 files in L6
  ASSERT_EQ("0,0,0,0,0,0,2", FilesPerLevel(0));

  ASSERT_EQ(trivial_move, 1);
  ASSERT_EQ(non_trivial_move, 0);

  // file 3 [ 0 => 200]
  for (int32_t i = 0; i < 200; i++) {
    values[i] = RandomString(&rnd, value_size);
    ASSERT_OK(Put(Key(i), values[i]));
  }
  ASSERT_OK(Flush());

  // 1 files in L0
  ASSERT_EQ("1,0,0,0,0,0,2", FilesPerLevel(0));
  ASSERT_OK(dbfull()->TEST_CompactRange(0, nullptr, nullptr, nullptr, false));
  ASSERT_OK(dbfull()->TEST_CompactRange(1, nullptr, nullptr, nullptr, false));
  ASSERT_OK(dbfull()->TEST_CompactRange(2, nullptr, nullptr, nullptr, false));
  ASSERT_OK(dbfull()->TEST_CompactRange(3, nullptr, nullptr, nullptr, false));
  ASSERT_OK(dbfull()->TEST_CompactRange(4, nullptr, nullptr, nullptr, false));
  // 2 files in L6, 1 file in L5
  ASSERT_EQ("0,0,0,0,0,1,2", FilesPerLevel(0));

  ASSERT_EQ(trivial_move, 6);
  ASSERT_EQ(non_trivial_move, 0);

  rocksdb::port::Thread threads([&] {
    compact_options.change_level = false;
    compact_options.exclusive_manual_compaction = false;
    std::string begin_string = Key(0);
    std::string end_string = Key(199);
    Slice begin(begin_string);
    Slice end(end_string);
    // First non-trivial compaction is triggered
    ASSERT_OK(db_->CompactRange(compact_options, &begin, &end));
  });

  TEST_SYNC_POINT("DBCompaction::ManualPartial:1");
  // file 4 [300 => 400)
  for (int32_t i = 300; i <= 400; i++) {
    values[i] = RandomString(&rnd, value_size);
    ASSERT_OK(Put(Key(i), values[i]));
  }
  ASSERT_OK(Flush());

  // file 5 [400 => 500)
  for (int32_t i = 400; i <= 500; i++) {
    values[i] = RandomString(&rnd, value_size);
    ASSERT_OK(Put(Key(i), values[i]));
  }
  ASSERT_OK(Flush());

  // file 6 [500 => 600)
  for (int32_t i = 500; i <= 600; i++) {
    values[i] = RandomString(&rnd, value_size);
    ASSERT_OK(Put(Key(i), values[i]));
  }
  // Second non-trivial compaction is triggered
  ASSERT_OK(Flush());

  // Before two non-trivial compactions are installed, there are 3 files in L0
  ASSERT_EQ("3,0,0,0,0,1,2", FilesPerLevel(0));
  TEST_SYNC_POINT("DBCompaction::ManualPartial:5");

  dbfull()->TEST_WaitForFlushMemTable();
  dbfull()->TEST_WaitForCompact();
  // After two non-trivial compactions are installed, there is 1 file in L6, and
  // 1 file in L1
  ASSERT_EQ("0,1,0,0,0,0,1", FilesPerLevel(0));
  threads.join();

  for (int32_t i = 0; i < 600; i++) {
    ASSERT_EQ(Get(Key(i)), values[i]);
  }
}

// Disable as the test is flaky.
TEST_F(DBVLogTest, DISABLED_ManualPartialFill) {
  int32_t trivial_move = 0;
  int32_t non_trivial_move = 0;
  rocksdb::SyncPoint::GetInstance()->SetCallBack(
      "DBImpl::BackgroundCompaction:TrivialMove",
      [&](void* arg) { trivial_move++; });
  rocksdb::SyncPoint::GetInstance()->SetCallBack(
      "DBImpl::BackgroundCompaction:NonTrivial",
      [&](void* arg) { non_trivial_move++; });
  bool first = true;
  bool second = true;
  rocksdb::SyncPoint::GetInstance()->LoadDependency(
      {{"DBCompaction::PartialFill:4", "DBCompaction::PartialFill:1"},
       {"DBCompaction::PartialFill:2", "DBCompaction::PartialFill:3"}});
  rocksdb::SyncPoint::GetInstance()->SetCallBack(
      "DBImpl::BackgroundCompaction:NonTrivial:AfterRun", [&](void* arg) {
        if (first) {
          TEST_SYNC_POINT("DBCompaction::PartialFill:4");
          first = false;
          TEST_SYNC_POINT("DBCompaction::PartialFill:3");
        } else if (second) {
        }
      });

  rocksdb::SyncPoint::GetInstance()->EnableProcessing();

  Options options = CurrentOptions();
  options.write_buffer_size = 10 * 1024 * 1024;
  options.max_bytes_for_level_multiplier = 2;
  options.num_levels = 4;
  options.level0_file_num_compaction_trigger = 3;
  options.max_background_compactions = 3;
#ifdef INDIRECT_VALUE_SUPPORT
  options.allow_trivial_move = true;
#endif

  DestroyAndReopen(options);
  // make sure all background compaction jobs can be scheduled
  auto stop_token =
      dbfull()->TEST_write_controler().GetCompactionPressureToken();
  int32_t value_size = 10 * 1024;  // 10 KB

  // Add 2 non-overlapping files
  Random rnd(301);
  std::map<int32_t, std::string> values;

  // file 1 [0 => 100]
  for (int32_t i = 0; i < 100; i++) {
    values[i] = RandomString(&rnd, value_size);
    ASSERT_OK(Put(Key(i), values[i]));
  }
  ASSERT_OK(Flush());

  // file 2 [100 => 300]
  for (int32_t i = 100; i < 300; i++) {
    values[i] = RandomString(&rnd, value_size);
    ASSERT_OK(Put(Key(i), values[i]));
  }
  ASSERT_OK(Flush());

  // 2 files in L0
  ASSERT_EQ("2", FilesPerLevel(0));
  CompactRangeOptions compact_options;
  compact_options.change_level = true;
  compact_options.target_level = 2;
  ASSERT_OK(db_->CompactRange(compact_options, nullptr, nullptr));
  // 2 files in L2
  ASSERT_EQ("0,0,2", FilesPerLevel(0));

  ASSERT_EQ(trivial_move, 1);
  ASSERT_EQ(non_trivial_move, 0);

  // file 3 [ 0 => 200]
  for (int32_t i = 0; i < 200; i++) {
    values[i] = RandomString(&rnd, value_size);
    ASSERT_OK(Put(Key(i), values[i]));
  }
  ASSERT_OK(Flush());

  // 2 files in L2, 1 in L0
  ASSERT_EQ("1,0,2", FilesPerLevel(0));
  ASSERT_OK(dbfull()->TEST_CompactRange(0, nullptr, nullptr, nullptr, false));
  // 2 files in L2, 1 in L1
  ASSERT_EQ("0,1,2", FilesPerLevel(0));

  ASSERT_EQ(trivial_move, 2);
  ASSERT_EQ(non_trivial_move, 0);

  rocksdb::port::Thread threads([&] {
    compact_options.change_level = false;
    compact_options.exclusive_manual_compaction = false;
    std::string begin_string = Key(0);
    std::string end_string = Key(199);
    Slice begin(begin_string);
    Slice end(end_string);
    ASSERT_OK(db_->CompactRange(compact_options, &begin, &end));
  });

  TEST_SYNC_POINT("DBCompaction::PartialFill:1");
  // Many files 4 [300 => 4300)
  for (int32_t i = 0; i <= 5; i++) {
    for (int32_t j = 300; j < 4300; j++) {
      if (j == 2300) {
        ASSERT_OK(Flush());
        dbfull()->TEST_WaitForFlushMemTable();
      }
      values[j] = RandomString(&rnd, value_size);
      ASSERT_OK(Put(Key(j), values[j]));
    }
  }

  // Verify level sizes
  uint64_t target_size = 4 * options.max_bytes_for_level_base;
  for (int32_t i = 1; i < options.num_levels; i++) {
    ASSERT_LE(SizeAtLevel(i), target_size);
    target_size = static_cast<uint64_t>(target_size *
                                        options.max_bytes_for_level_multiplier);
  }

  TEST_SYNC_POINT("DBCompaction::PartialFill:2");
  dbfull()->TEST_WaitForFlushMemTable();
  dbfull()->TEST_WaitForCompact();
  threads.join();

  for (int32_t i = 0; i < 4300; i++) {
    ASSERT_EQ(Get(Key(i)), values[i]);
  }
}

TEST_F(DBVLogTest, DeleteFileRange) {
  Options options = CurrentOptions();
  options.write_buffer_size = 10 * 1024 * 1024;
  options.max_bytes_for_level_multiplier = 2;
  options.num_levels = 4;
  options.level0_file_num_compaction_trigger = 3;
  options.max_background_compactions = 3;
#ifdef INDIRECT_VALUE_SUPPORT
  options.allow_trivial_move = true;
#endif

  DestroyAndReopen(options);
  int32_t value_size = 10 * 1024;  // 10 KB

  // Add 2 non-overlapping files
  Random rnd(301);
  std::map<int32_t, std::string> values;

  // file 1 [0 => 100]
  for (int32_t i = 0; i < 100; i++) {
    values[i] = RandomString(&rnd, value_size);
    ASSERT_OK(PutBig(i, value_size, values[i]));
  }
  ASSERT_OK(Flush());

  // file 2 [100 => 300]
  for (int32_t i = 100; i < 300; i++) {
    values[i] = RandomString(&rnd, value_size);
    ASSERT_OK(PutBig(i, value_size, values[i]));
  }
  ASSERT_OK(Flush());

  // 2 files in L0
  ASSERT_EQ("2", FilesPerLevel(0));
  CompactRangeOptions compact_options;
  compact_options.change_level = true;
  compact_options.target_level = 2;
  ASSERT_OK(db_->CompactRange(compact_options, nullptr, nullptr));
  // 2 files in L2
  ASSERT_EQ("0,0,2", FilesPerLevel(0));
  // file 3 [ 0 => 200]
  for (int32_t i = 0; i < 200; i++) {
    values[i] = RandomString(&rnd, value_size);
    ASSERT_OK(PutBig(i, value_size, values[i]));
  }
  ASSERT_OK(Flush());

  // Many files 4 [300 => 4300)
  for (int32_t i = 0; i <= 5; i++) {
    for (int32_t j = 300; j < 4300; j++) {
      if (j == 2300) {
        ASSERT_OK(Flush());
        dbfull()->TEST_WaitForFlushMemTable();
      }
      values[j] = RandomString(&rnd, value_size);
      ASSERT_OK(PutBig(j, value_size, values[j]));
    }
  }
  ASSERT_OK(Flush());
  dbfull()->TEST_WaitForFlushMemTable();
  dbfull()->TEST_WaitForCompact();

  // Verify level sizes
  uint64_t target_size = 4 * options.max_bytes_for_level_base;
  for (int32_t i = 1; i < options.num_levels; i++) {
    ASSERT_LE(SizeAtLevel(i), target_size);
    target_size = static_cast<uint64_t>(target_size *
                                        options.max_bytes_for_level_multiplier);
  }

  size_t old_num_files = CountFiles();
  std::string begin_string = KeyBig(1000, value_size);
  std::string end_string = KeyBig(2000, value_size);
  Slice begin(begin_string);
  Slice end(end_string);
  ASSERT_OK(DeleteFilesInRange(db_, db_->DefaultColumnFamily(), &begin, &end));

  int32_t deleted_count = 0;
  for (int32_t i = 0; i < 4300; i++) {
    if (i < 1000 || i > 2000) {
      ASSERT_EQ(Get(KeyBig(i, value_size)), ValueBig(values[i]));
    } else {
      ReadOptions roptions;
      std::string result;
      Status s = db_->Get(roptions, KeyBig(i, value_size), &result);
      ASSERT_TRUE(s.IsNotFound() || s.ok());
      if (s.IsNotFound()) {
        deleted_count++;
      }
    }
  }
  ASSERT_GT(deleted_count, 0);
  begin_string = KeyBig(5000, value_size);
  end_string = KeyBig(6000, value_size);
  Slice begin1(begin_string);
  Slice end1(end_string);
  // Try deleting files in range which contain no keys
  ASSERT_OK(
      DeleteFilesInRange(db_, db_->DefaultColumnFamily(), &begin1, &end1));

  // Push data from level 0 to level 1 to force all data to be deleted
  // Note that we don't delete level 0 files
  compact_options.change_level = true;
  compact_options.target_level = 1;
  ASSERT_OK(dbfull()->TEST_CompactRange(0, nullptr, nullptr));

  ASSERT_OK(
      DeleteFilesInRange(db_, db_->DefaultColumnFamily(), nullptr, nullptr));

  int32_t deleted_count2 = 0;
  for (int32_t i = 0; i < 4300; i++) {
    ReadOptions roptions;
    std::string result;
    Status s = db_->Get(roptions, KeyBig(i, value_size), &result);
    ASSERT_TRUE(s.IsNotFound());
    deleted_count2++;
  }
  ASSERT_GT(deleted_count2, deleted_count);
  size_t new_num_files = CountFiles();
  ASSERT_GT(old_num_files, new_num_files);
}

TEST_P(DBVLogTestWithParam, TrivialMoveToLastLevelWithFiles) {
  int32_t trivial_move = 0;
  int32_t non_trivial_move = 0;
  rocksdb::SyncPoint::GetInstance()->SetCallBack(
      "DBImpl::BackgroundCompaction:TrivialMove",
      [&](void* arg) { trivial_move++; });
  rocksdb::SyncPoint::GetInstance()->SetCallBack(
      "DBImpl::BackgroundCompaction:NonTrivial",
      [&](void* arg) { non_trivial_move++; });
  rocksdb::SyncPoint::GetInstance()->EnableProcessing();

  Options options = CurrentOptions();
  options.write_buffer_size = 100000000;
  options.max_subcompactions = max_subcompactions_;
#ifdef INDIRECT_VALUE_SUPPORT
  options.allow_trivial_move = true;
#endif
  DestroyAndReopen(options);

  int32_t value_size = 10 * 1024;  // 10 KB

  Random rnd(301);
  std::vector<std::string> values;
  // File with keys [ 0 => 99 ]
  for (int i = 0; i < 100; i++) {
    values.push_back(RandomString(&rnd, value_size));
    ASSERT_OK(PutBig(i, value_size, values[i]));
  }
  ASSERT_OK(Flush());

  ASSERT_EQ("1", FilesPerLevel(0));
  // Compaction will do L0=>L1 (trivial move) then move L1 files to L3
  CompactRangeOptions compact_options;
  compact_options.change_level = true;
  compact_options.target_level = 3;
  compact_options.exclusive_manual_compaction = exclusive_manual_compaction_;
  ASSERT_OK(db_->CompactRange(compact_options, nullptr, nullptr));
  ASSERT_EQ("0,0,0,1", FilesPerLevel(0));
  ASSERT_EQ(trivial_move, 1);
  ASSERT_EQ(non_trivial_move, 0);

  // File with keys [ 100 => 199 ]
  for (int i = 100; i < 200; i++) {
    values.push_back(RandomString(&rnd, value_size));
    ASSERT_OK(PutBig(i, value_size, values[i]));
  }
  ASSERT_OK(Flush());

  ASSERT_EQ("1,0,0,1", FilesPerLevel(0));
  CompactRangeOptions cro;
  cro.exclusive_manual_compaction = exclusive_manual_compaction_;
  // Compaction will do L0=>L1 L1=>L2 L2=>L3 (3 trivial moves)
  ASSERT_OK(db_->CompactRange(cro, nullptr, nullptr));
  ASSERT_EQ("0,0,0,2", FilesPerLevel(0));
  ASSERT_EQ(trivial_move, 4);
  ASSERT_EQ(non_trivial_move, 0);

  for (int i = 0; i < 200; i++) {
    ASSERT_EQ(Get(KeyBig(i, value_size)), ValueBig(values[i]));
  }

  rocksdb::SyncPoint::GetInstance()->DisableProcessing();
}

TEST_P(DBVLogTestWithParam, LevelCompactionThirdPath) {
  Options options = CurrentOptions();
  options.db_paths.emplace_back(dbname_, 500 * 1024);
  options.db_paths.emplace_back(dbname_ + "_2", 4 * 1024 * 1024);
  options.db_paths.emplace_back(dbname_ + "_3", 1024 * 1024 * 1024);
  options.memtable_factory.reset(
      new SpecialSkipListFactory(KNumKeysByGenerateNewFile - 1));
  options.compaction_style = kCompactionStyleLevel;
  options.write_buffer_size = 110 << 10;  // 110KB
  options.arena_block_size = 4 << 10;
  options.level0_file_num_compaction_trigger = 2;
  options.num_levels = 4;
  options.max_bytes_for_level_base = 400 * 1024;
  options.max_subcompactions = max_subcompactions_;
  //  options = CurrentOptions(options);
#ifdef INDIRECT_VALUE_SUPPORT
  const int largevaluesize = 16;  // RandomFileBig produces value length of either 1 or this
#else
  const int largevaluesize = 990;  // RandomFileBig produces value length of either 1 or this
#endif

  std::vector<std::string> filenames;
  env_->GetChildren(options.db_paths[1].path, &filenames);
  // Delete archival files.
  for (size_t i = 0; i < filenames.size(); ++i) {
    env_->DeleteFile(options.db_paths[1].path + "/" + filenames[i]);
  }
  env_->DeleteDir(options.db_paths[1].path);
  Reopen(options);

  Random rnd(301);
  int key_idx = 0;

  // First three 110KB files are not going to second path.
  // After that, (100K, 200K)
  for (int num = 0; num < 3; num++) {
    GenerateNewFileBig(&rnd, &key_idx);
  }

  // Another 110KB triggers a compaction to 400K file to fill up first path
  GenerateNewFileBig(&rnd, &key_idx);
  ASSERT_EQ(3, GetSstFileCount(options.db_paths[1].path));

  // (1, 4)
  GenerateNewFileBig(&rnd, &key_idx);
  ASSERT_EQ("1,4", FilesPerLevel(0));
  ASSERT_EQ(4, GetSstFileCount(options.db_paths[1].path));
  ASSERT_EQ(1, GetSstFileCount(dbname_));

  // (1, 4, 1)
  GenerateNewFileBig(&rnd, &key_idx);
  ASSERT_EQ("1,4,1", FilesPerLevel(0));
  ASSERT_EQ(1, GetSstFileCount(options.db_paths[2].path));
  ASSERT_EQ(4, GetSstFileCount(options.db_paths[1].path));
  ASSERT_EQ(1, GetSstFileCount(dbname_));

  // (1, 4, 2)
  GenerateNewFileBig(&rnd, &key_idx);
  ASSERT_EQ("1,4,2", FilesPerLevel(0));
  ASSERT_EQ(2, GetSstFileCount(options.db_paths[2].path));
  ASSERT_EQ(4, GetSstFileCount(options.db_paths[1].path));
  ASSERT_EQ(1, GetSstFileCount(dbname_));

  // (1, 4, 3)
  GenerateNewFileBig(&rnd, &key_idx);
  ASSERT_EQ("1,4,3", FilesPerLevel(0));
  ASSERT_EQ(3, GetSstFileCount(options.db_paths[2].path));
  ASSERT_EQ(4, GetSstFileCount(options.db_paths[1].path));
  ASSERT_EQ(1, GetSstFileCount(dbname_));

  // (1, 4, 4)
  GenerateNewFileBig(&rnd, &key_idx);
  ASSERT_EQ("1,4,4", FilesPerLevel(0));
  ASSERT_EQ(4, GetSstFileCount(options.db_paths[2].path));
  ASSERT_EQ(4, GetSstFileCount(options.db_paths[1].path));
  ASSERT_EQ(1, GetSstFileCount(dbname_));

  // (1, 4, 5)
  GenerateNewFileBig(&rnd, &key_idx);
  ASSERT_EQ("1,4,5", FilesPerLevel(0));
  ASSERT_EQ(5, GetSstFileCount(options.db_paths[2].path));
  ASSERT_EQ(4, GetSstFileCount(options.db_paths[1].path));
  ASSERT_EQ(1, GetSstFileCount(dbname_));

  // (1, 4, 6)
  GenerateNewFileBig(&rnd, &key_idx);
  ASSERT_EQ("1,4,6", FilesPerLevel(0));
  ASSERT_EQ(6, GetSstFileCount(options.db_paths[2].path));
  ASSERT_EQ(4, GetSstFileCount(options.db_paths[1].path));
  ASSERT_EQ(1, GetSstFileCount(dbname_));

  // (1, 4, 7)
  GenerateNewFileBig(&rnd, &key_idx);
  ASSERT_EQ("1,4,7", FilesPerLevel(0));
  ASSERT_EQ(7, GetSstFileCount(options.db_paths[2].path));
  ASSERT_EQ(4, GetSstFileCount(options.db_paths[1].path));
  ASSERT_EQ(1, GetSstFileCount(dbname_));

  // (1, 4, 8)
  GenerateNewFileBig(&rnd, &key_idx);
  ASSERT_EQ("1,4,8", FilesPerLevel(0));
  ASSERT_EQ(8, GetSstFileCount(options.db_paths[2].path));
  ASSERT_EQ(4, GetSstFileCount(options.db_paths[1].path));
  ASSERT_EQ(1, GetSstFileCount(dbname_));

  for (int i = 0; i < key_idx; i++) {
    auto v = Get(KeyBigNewFile(i,i%100));
    ASSERT_NE(v, "NOT_FOUND");
    ASSERT_TRUE(v.size() == 1 || v.size() == largevaluesize);
  }

  Reopen(options);

  for (int i = 0; i < key_idx; i++) {
    auto v = Get(KeyBigNewFile(i,i%100));
    ASSERT_NE(v, "NOT_FOUND");
    ASSERT_TRUE(v.size() == 1 || v.size() == largevaluesize);
  }

  Destroy(options);
}

TEST_P(DBVLogTestWithParam, LevelCompactionPathUse) {
  Options options = CurrentOptions();
  options.db_paths.emplace_back(dbname_, 500 * 1024);
  options.db_paths.emplace_back(dbname_ + "_2", 4 * 1024 * 1024);
  options.db_paths.emplace_back(dbname_ + "_3", 1024 * 1024 * 1024);
  options.memtable_factory.reset(
      new SpecialSkipListFactory(KNumKeysByGenerateNewFile - 1));
  options.compaction_style = kCompactionStyleLevel;
  options.write_buffer_size = 110 << 10;  // 110KB
  options.arena_block_size = 4 << 10;
  options.level0_file_num_compaction_trigger = 2;
  options.num_levels = 4;
  options.max_bytes_for_level_base = 400 * 1024;
  options.max_subcompactions = max_subcompactions_;
  //  options = CurrentOptions(options);

  std::vector<std::string> filenames;
  env_->GetChildren(options.db_paths[1].path, &filenames);
  // Delete archival files.
  for (size_t i = 0; i < filenames.size(); ++i) {
    env_->DeleteFile(options.db_paths[1].path + "/" + filenames[i]);
  }
  env_->DeleteDir(options.db_paths[1].path);
  Reopen(options);

  Random rnd(301);
  int key_idx = 0;

  // Always gets compacted into 1 Level1 file,
  // 0/1 Level 0 file
  for (int num = 0; num < 3; num++) {
    key_idx = 0;
    GenerateNewFile(&rnd, &key_idx);
  }

  key_idx = 0;
  GenerateNewFile(&rnd, &key_idx);
  ASSERT_EQ(1, GetSstFileCount(options.db_paths[1].path));

  key_idx = 0;
  GenerateNewFile(&rnd, &key_idx);
  ASSERT_EQ("1,1", FilesPerLevel(0));
  ASSERT_EQ(1, GetSstFileCount(options.db_paths[1].path));
  ASSERT_EQ(1, GetSstFileCount(dbname_));

  key_idx = 0;
  GenerateNewFile(&rnd, &key_idx);
  ASSERT_EQ("0,1", FilesPerLevel(0));
  ASSERT_EQ(0, GetSstFileCount(options.db_paths[2].path));
  ASSERT_EQ(1, GetSstFileCount(options.db_paths[1].path));
  ASSERT_EQ(0, GetSstFileCount(dbname_));

  key_idx = 0;
  GenerateNewFile(&rnd, &key_idx);
  ASSERT_EQ("1,1", FilesPerLevel(0));
  ASSERT_EQ(0, GetSstFileCount(options.db_paths[2].path));
  ASSERT_EQ(1, GetSstFileCount(options.db_paths[1].path));
  ASSERT_EQ(1, GetSstFileCount(dbname_));

  key_idx = 0;
  GenerateNewFile(&rnd, &key_idx);
  ASSERT_EQ("0,1", FilesPerLevel(0));
  ASSERT_EQ(0, GetSstFileCount(options.db_paths[2].path));
  ASSERT_EQ(1, GetSstFileCount(options.db_paths[1].path));
  ASSERT_EQ(0, GetSstFileCount(dbname_));

  key_idx = 0;
  GenerateNewFile(&rnd, &key_idx);
  ASSERT_EQ("1,1", FilesPerLevel(0));
  ASSERT_EQ(0, GetSstFileCount(options.db_paths[2].path));
  ASSERT_EQ(1, GetSstFileCount(options.db_paths[1].path));
  ASSERT_EQ(1, GetSstFileCount(dbname_));

  key_idx = 0;
  GenerateNewFile(&rnd, &key_idx);
  ASSERT_EQ("0,1", FilesPerLevel(0));
  ASSERT_EQ(0, GetSstFileCount(options.db_paths[2].path));
  ASSERT_EQ(1, GetSstFileCount(options.db_paths[1].path));
  ASSERT_EQ(0, GetSstFileCount(dbname_));

  key_idx = 0;
  GenerateNewFile(&rnd, &key_idx);
  ASSERT_EQ("1,1", FilesPerLevel(0));
  ASSERT_EQ(0, GetSstFileCount(options.db_paths[2].path));
  ASSERT_EQ(1, GetSstFileCount(options.db_paths[1].path));
  ASSERT_EQ(1, GetSstFileCount(dbname_));

  key_idx = 0;
  GenerateNewFile(&rnd, &key_idx);
  ASSERT_EQ("0,1", FilesPerLevel(0));
  ASSERT_EQ(0, GetSstFileCount(options.db_paths[2].path));
  ASSERT_EQ(1, GetSstFileCount(options.db_paths[1].path));
  ASSERT_EQ(0, GetSstFileCount(dbname_));

  key_idx = 0;
  GenerateNewFile(&rnd, &key_idx);
  ASSERT_EQ("1,1", FilesPerLevel(0));
  ASSERT_EQ(0, GetSstFileCount(options.db_paths[2].path));
  ASSERT_EQ(1, GetSstFileCount(options.db_paths[1].path));
  ASSERT_EQ(1, GetSstFileCount(dbname_));

  for (int i = 0; i < key_idx; i++) {
    auto v = Get(Key(i));
    ASSERT_NE(v, "NOT_FOUND");
    ASSERT_TRUE(v.size() == 1 || v.size() == 990);
  }

  Reopen(options);

  for (int i = 0; i < key_idx; i++) {
    auto v = Get(Key(i));
    ASSERT_NE(v, "NOT_FOUND");
    ASSERT_TRUE(v.size() == 1 || v.size() == 990);
  }

  Destroy(options);
}

TEST_P(DBVLogTestWithParam, ConvertCompactionStyle) {
  Random rnd(301);
  int max_key_level_insert = 200;
  int max_key_universal_insert = 600;

  // Stage 1: generate a db with level compaction
  Options options = CurrentOptions();
  options.write_buffer_size = 110 << 10;  // 110KB
  options.arena_block_size = 4 << 10;
  options.num_levels = 4;
  options.level0_file_num_compaction_trigger = 3;
  options.max_bytes_for_level_base = 500 << 10;  // 500KB
  options.max_bytes_for_level_multiplier = 1;
  options.target_file_size_base = 200 << 10;  // 200KB
  options.target_file_size_multiplier = 1;
  options.max_subcompactions = max_subcompactions_;
  CreateAndReopenWithCF({"pikachu"}, options);

  for (int i = 0; i <= max_key_level_insert; i++) {
    // each value is 10K
    ASSERT_OK(Put(1, Key(i), RandomString(&rnd, 10000)));
  }
  ASSERT_OK(Flush(1));
  dbfull()->TEST_WaitForCompact();

  ASSERT_GT(TotalTableFiles(1, 4), 1);
  int non_level0_num_files = 0;
  for (int i = 1; i < options.num_levels; i++) {
    non_level0_num_files += NumTableFilesAtLevel(i, 1);
  }
  ASSERT_GT(non_level0_num_files, 0);

  // Stage 2: reopen with universal compaction - should fail
  options = CurrentOptions();
  options.compaction_style = kCompactionStyleUniversal;
  options.num_levels = 1;
  options = CurrentOptions(options);
  Status s = TryReopenWithColumnFamilies({"default", "pikachu"}, options);
  ASSERT_TRUE(s.IsInvalidArgument());

  // Stage 3: compact into a single file and move the file to level 0
  options = CurrentOptions();
  options.disable_auto_compactions = true;
  options.target_file_size_base = INT_MAX;
  options.target_file_size_multiplier = 1;
  options.max_bytes_for_level_base = INT_MAX;
  options.max_bytes_for_level_multiplier = 1;
  options.num_levels = 4;
  options = CurrentOptions(options);
  ReopenWithColumnFamilies({"default", "pikachu"}, options);

  CompactRangeOptions compact_options;
  compact_options.change_level = true;
  compact_options.target_level = 0;
  compact_options.bottommost_level_compaction =
      BottommostLevelCompaction::kForce;
  compact_options.exclusive_manual_compaction = exclusive_manual_compaction_;
  dbfull()->CompactRange(compact_options, handles_[1], nullptr, nullptr);

  // Only 1 file in L0
  ASSERT_EQ("1", FilesPerLevel(1));

  // Stage 4: re-open in universal compaction style and do some db operations
  options = CurrentOptions();
  options.compaction_style = kCompactionStyleUniversal;
  options.num_levels = 4;
  options.write_buffer_size = 110 << 10;  // 110KB
  options.arena_block_size = 4 << 10;
  options.level0_file_num_compaction_trigger = 3;
  options = CurrentOptions(options);
  ReopenWithColumnFamilies({"default", "pikachu"}, options);

  options.num_levels = 1;
  ReopenWithColumnFamilies({"default", "pikachu"}, options);

  for (int i = max_key_level_insert / 2; i <= max_key_universal_insert; i++) {
    ASSERT_OK(Put(1, Key(i), RandomString(&rnd, 10000)));
  }
  dbfull()->Flush(FlushOptions());
  ASSERT_OK(Flush(1));
  dbfull()->TEST_WaitForCompact();

  for (int i = 1; i < options.num_levels; i++) {
    ASSERT_EQ(NumTableFilesAtLevel(i, 1), 0);
  }

  // verify keys inserted in both level compaction style and universal
  // compaction style
  std::string keys_in_db;
  Iterator* iter = dbfull()->NewIterator(ReadOptions(), handles_[1]);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    keys_in_db.append(iter->key().ToString());
    keys_in_db.push_back(',');
  }
  delete iter;

  std::string expected_keys;
  for (int i = 0; i <= max_key_universal_insert; i++) {
    expected_keys.append(Key(i));
    expected_keys.push_back(',');
  }

  ASSERT_EQ(keys_in_db, expected_keys);
}

TEST_F(DBVLogTest, L0_CompactionBug_Issue44_a) {
#ifndef INDIRECT_VALUE_SUPPORT  // turn off for indirect, because Contents can't see column family
  do {
    CreateAndReopenWithCF({"pikachu"}, CurrentOptions());
    ASSERT_OK(Put(1, "b", "v"));
    ReopenWithColumnFamilies({"default", "pikachu"}, CurrentOptions());
    ASSERT_OK(Delete(1, "b"));
    ASSERT_OK(Delete(1, "a"));
    ReopenWithColumnFamilies({"default", "pikachu"}, CurrentOptions());
    ASSERT_OK(Delete(1, "a"));
    ReopenWithColumnFamilies({"default", "pikachu"}, CurrentOptions());
    ASSERT_OK(Put(1, "a", "v"));
    ReopenWithColumnFamilies({"default", "pikachu"}, CurrentOptions());
    ReopenWithColumnFamilies({"default", "pikachu"}, CurrentOptions());
    ASSERT_EQ("(a->v)", Contents(1));
    env_->SleepForMicroseconds(1000000);  // Wait for compaction to finish
    ASSERT_EQ("(a->v)", Contents(1));
  } while (ChangeCompactOptions());
#endif
}

TEST_F(DBVLogTest, L0_CompactionBug_Issue44_b) {
#ifndef INDIRECT_VALUE_SUPPORT  // turn off for indirect, because Contents can't see column family
  do {
    CreateAndReopenWithCF({"pikachu"}, CurrentOptions());
    Put(1, "", "");
    ReopenWithColumnFamilies({"default", "pikachu"}, CurrentOptions());
    Delete(1, "e");
    Put(1, "", "");
    ReopenWithColumnFamilies({"default", "pikachu"}, CurrentOptions());
    Put(1, "c", "cv");
    ReopenWithColumnFamilies({"default", "pikachu"}, CurrentOptions());
    Put(1, "", "");
    ReopenWithColumnFamilies({"default", "pikachu"}, CurrentOptions());
    Put(1, "", "");
    env_->SleepForMicroseconds(1000000);  // Wait for compaction to finish
    ReopenWithColumnFamilies({"default", "pikachu"}, CurrentOptions());
    Put(1, "d", "dv");
    ReopenWithColumnFamilies({"default", "pikachu"}, CurrentOptions());
    Put(1, "", "");
    ReopenWithColumnFamilies({"default", "pikachu"}, CurrentOptions());
    Delete(1, "d");
    Delete(1, "b");
    ReopenWithColumnFamilies({"default", "pikachu"}, CurrentOptions());
    ASSERT_EQ("(->)(c->cv)", Contents(1));
    env_->SleepForMicroseconds(1000000);  // Wait for compaction to finish
    ASSERT_EQ("(->)(c->cv)", Contents(1));
  } while (ChangeCompactOptions());
#endif
}

TEST_F(DBVLogTest, ManualAutoRace) {
  CreateAndReopenWithCF({"pikachu"}, CurrentOptions());
  rocksdb::SyncPoint::GetInstance()->LoadDependency(
      {{"DBImpl::BGWorkCompaction", "DBVLogTest::ManualAutoRace:1"},
       {"DBImpl::RunManualCompaction:WaitScheduled",
        "BackgroundCallCompaction:0"}});

  rocksdb::SyncPoint::GetInstance()->EnableProcessing();

  Put(1, "foo", "");
  Put(1, "bar", "");
  Flush(1);
  Put(1, "foo", "");
  Put(1, "bar", "");
  // Generate four files in CF 0, which should trigger an auto compaction
  Put("foo", "");
  Put("bar", "");
  Flush();
  Put("foo", "");
  Put("bar", "");
  Flush();
  Put("foo", "");
  Put("bar", "");
  Flush();
  Put("foo", "");
  Put("bar", "");
  Flush();

  // The auto compaction is scheduled but waited until here
  TEST_SYNC_POINT("DBVLogTest::ManualAutoRace:1");
  // The auto compaction will wait until the manual compaction is registerd
  // before processing so that it will be cancelled.
  dbfull()->CompactRange(CompactRangeOptions(), handles_[1], nullptr, nullptr);
  ASSERT_EQ("0,1", FilesPerLevel(1));

  // Eventually the cancelled compaction will be rescheduled and executed.
  dbfull()->TEST_WaitForCompact();
  ASSERT_EQ("0,1", FilesPerLevel(0));
  rocksdb::SyncPoint::GetInstance()->DisableProcessing();
}

TEST_P(DBVLogTestWithParam, ManualCompaction) {
  Options options = CurrentOptions();
  options.max_subcompactions = max_subcompactions_;
  options.statistics = rocksdb::CreateDBStatistics();
  CreateAndReopenWithCF({"pikachu"}, options);

  // iter - 0 with 7 levels
  // iter - 1 with 3 levels
  for (int iter = 0; iter < 2; ++iter) {
    MakeTables(3, "p", "q", 1);
    ASSERT_EQ("1,1,1", FilesPerLevel(1));

    // Compaction range falls before files
    Compact(1, "", "c");
    ASSERT_EQ("1,1,1", FilesPerLevel(1));

    // Compaction range falls after files
    Compact(1, "r", "z");
    ASSERT_EQ("1,1,1", FilesPerLevel(1));

    // Compaction range overlaps files
    Compact(1, "p1", "p9");
    ASSERT_EQ("0,0,1", FilesPerLevel(1));

    // Populate a different range
    MakeTables(3, "c", "e", 1);
    ASSERT_EQ("1,1,2", FilesPerLevel(1));

    // Compact just the new range
    Compact(1, "b", "f");
    ASSERT_EQ("0,0,2", FilesPerLevel(1));

    // Compact all
    MakeTables(1, "a", "z", 1);
    ASSERT_EQ("1,0,2", FilesPerLevel(1));

    uint64_t prev_block_cache_add =
        options.statistics->getTickerCount(BLOCK_CACHE_ADD);
    CompactRangeOptions cro;
    cro.exclusive_manual_compaction = exclusive_manual_compaction_;
    db_->CompactRange(cro, handles_[1], nullptr, nullptr);
    // Verify manual compaction doesn't fill block cache
    ASSERT_EQ(prev_block_cache_add,
              options.statistics->getTickerCount(BLOCK_CACHE_ADD));

    ASSERT_EQ("0,0,1", FilesPerLevel(1));

    if (iter == 0) {
      options = CurrentOptions();
      options.num_levels = 3;
      options.create_if_missing = true;
      options.statistics = rocksdb::CreateDBStatistics();
      DestroyAndReopen(options);
      CreateAndReopenWithCF({"pikachu"}, options);
    }
  }
}


TEST_P(DBVLogTestWithParam, ManualLevelCompactionOutputPathId) {
  Options options = CurrentOptions();
  options.db_paths.emplace_back(dbname_ + "_2", 2 * 10485760);
  options.db_paths.emplace_back(dbname_ + "_3", 100 * 10485760);
  options.db_paths.emplace_back(dbname_ + "_4", 120 * 10485760);
  options.max_subcompactions = max_subcompactions_;
  CreateAndReopenWithCF({"pikachu"}, options);

  // iter - 0 with 7 levels
  // iter - 1 with 3 levels
  for (int iter = 0; iter < 2; ++iter) {
    for (int i = 0; i < 3; ++i) {
      ASSERT_OK(Put(1, "p", "begin"));
      ASSERT_OK(Put(1, "q", "end"));
      ASSERT_OK(Flush(1));
    }
    ASSERT_EQ("3", FilesPerLevel(1));
    ASSERT_EQ(3, GetSstFileCount(options.db_paths[0].path));
    ASSERT_EQ(0, GetSstFileCount(dbname_));

    // Compaction range falls before files
    Compact(1, "", "c");
    ASSERT_EQ("3", FilesPerLevel(1));

    // Compaction range falls after files
    Compact(1, "r", "z");
    ASSERT_EQ("3", FilesPerLevel(1));

    // Compaction range overlaps files
    Compact(1, "p1", "p9", 1);
    ASSERT_EQ("0,1", FilesPerLevel(1));
    ASSERT_EQ(1, GetSstFileCount(options.db_paths[1].path));
    ASSERT_EQ(0, GetSstFileCount(options.db_paths[0].path));
    ASSERT_EQ(0, GetSstFileCount(dbname_));

    // Populate a different range
    for (int i = 0; i < 3; ++i) {
      ASSERT_OK(Put(1, "c", "begin"));
      ASSERT_OK(Put(1, "e", "end"));
      ASSERT_OK(Flush(1));
    }
    ASSERT_EQ("3,1", FilesPerLevel(1));

    // Compact just the new range
    Compact(1, "b", "f", 1);
    ASSERT_EQ("0,2", FilesPerLevel(1));
    ASSERT_EQ(2, GetSstFileCount(options.db_paths[1].path));
    ASSERT_EQ(0, GetSstFileCount(options.db_paths[0].path));
    ASSERT_EQ(0, GetSstFileCount(dbname_));

    // Compact all
    ASSERT_OK(Put(1, "a", "begin"));
    ASSERT_OK(Put(1, "z", "end"));
    ASSERT_OK(Flush(1));
    ASSERT_EQ("1,2", FilesPerLevel(1));
    ASSERT_EQ(2, GetSstFileCount(options.db_paths[1].path));
    ASSERT_EQ(1, GetSstFileCount(options.db_paths[0].path));
    CompactRangeOptions compact_options;
    compact_options.target_path_id = 1;
    compact_options.exclusive_manual_compaction = exclusive_manual_compaction_;
    db_->CompactRange(compact_options, handles_[1], nullptr, nullptr);

    ASSERT_EQ("0,1", FilesPerLevel(1));
    ASSERT_EQ(1, GetSstFileCount(options.db_paths[1].path));
    ASSERT_EQ(0, GetSstFileCount(options.db_paths[0].path));
    ASSERT_EQ(0, GetSstFileCount(dbname_));

    if (iter == 0) {
      DestroyAndReopen(options);
      options = CurrentOptions();
      options.db_paths.emplace_back(dbname_ + "_2", 2 * 10485760);
      options.db_paths.emplace_back(dbname_ + "_3", 100 * 10485760);
      options.db_paths.emplace_back(dbname_ + "_4", 120 * 10485760);
      options.max_background_flushes = 1;
      options.num_levels = 3;
      options.create_if_missing = true;
      CreateAndReopenWithCF({"pikachu"}, options);
    }
  }
}

TEST_F(DBVLogTest, FilesDeletedAfterCompaction) {
  do {
    CreateAndReopenWithCF({"pikachu"}, CurrentOptions());
    ASSERT_OK(Put(1, "foo", "v2"));
    Compact(1, "a", "z");
    const size_t num_files = CountLiveFiles();
    for (int i = 0; i < 10; i++) {
      ASSERT_OK(Put(1, "foo", "v2"));
      Compact(1, "a", "z");
    }
    ASSERT_EQ(CountLiveFiles(), num_files);
  } while (ChangeCompactOptions());
}

// Check level comapction with compact files
TEST_P(DBVLogTestWithParam, DISABLED_CompactFilesOnLevelCompaction) {
  const int kTestKeySize = 16;
  const int kTestValueSize = 984;
  const int kEntrySize = kTestKeySize + kTestValueSize;
  const int kEntriesPerBuffer = 100;
  Options options;
  options.create_if_missing = true;
  options.write_buffer_size = kEntrySize * kEntriesPerBuffer;
  options.compaction_style = kCompactionStyleLevel;
  options.target_file_size_base = options.write_buffer_size;
  options.max_bytes_for_level_base = options.target_file_size_base * 2;
  options.level0_stop_writes_trigger = 2;
  options.max_bytes_for_level_multiplier = 2;
  options.compression = kNoCompression;
  options.max_subcompactions = max_subcompactions_;
  options = CurrentOptions(options);
  CreateAndReopenWithCF({"pikachu"}, options);

  Random rnd(301);
  for (int key = 64 * kEntriesPerBuffer; key >= 0; --key) {
    ASSERT_OK(Put(1, ToString(key), RandomString(&rnd, kTestValueSize)));
  }
  dbfull()->TEST_WaitForFlushMemTable(handles_[1]);
  dbfull()->TEST_WaitForCompact();

  ColumnFamilyMetaData cf_meta;
  dbfull()->GetColumnFamilyMetaData(handles_[1], &cf_meta);
  int output_level = static_cast<int>(cf_meta.levels.size()) - 1;
  for (int file_picked = 5; file_picked > 0; --file_picked) {
    std::set<std::string> overlapping_file_names;
    std::vector<std::string> compaction_input_file_names;
    for (int f = 0; f < file_picked; ++f) {
      int level = 0;
      auto file_meta = PickFileRandomly(cf_meta, &rnd, &level);
      compaction_input_file_names.push_back(file_meta->name);
      GetOverlappingFileNumbersForLevelCompaction(
          cf_meta, options.comparator, level, output_level,
          file_meta, &overlapping_file_names);
    }

    ASSERT_OK(dbfull()->CompactFiles(
        CompactionOptions(), handles_[1],
        compaction_input_file_names,
        output_level));

    // Make sure all overlapping files do not exist after compaction
    dbfull()->GetColumnFamilyMetaData(handles_[1], &cf_meta);
    VerifyCompactionResult(cf_meta, overlapping_file_names);
  }

  // make sure all key-values are still there.
  for (int key = 64 * kEntriesPerBuffer; key >= 0; --key) {
    ASSERT_NE(Get(1, ToString(key)), "NOT_FOUND");
  }
}

TEST_P(DBVLogTestWithParam, PartialCompactionFailure) {
  Options options;
  const int kKeySize = 16;
  const int kKvSize = 1000;
  const int kKeysPerBuffer = 100;
  const int kNumL1Files = 5;
  options.create_if_missing = true;
  options.write_buffer_size = kKeysPerBuffer * kKvSize;
  options.max_write_buffer_number = 2;
  options.target_file_size_base =
      options.write_buffer_size *
      (options.max_write_buffer_number - 1);
  options.level0_file_num_compaction_trigger = kNumL1Files;
  options.max_bytes_for_level_base =
      options.level0_file_num_compaction_trigger *
      options.target_file_size_base;
  options.max_bytes_for_level_multiplier = 2;
  options.compression = kNoCompression;
  options.max_subcompactions = max_subcompactions_;

  env_->SetBackgroundThreads(1, Env::HIGH);
  env_->SetBackgroundThreads(1, Env::LOW);
  // stop the compaction thread until we simulate the file creation failure.
  test::SleepingBackgroundTask sleeping_task_low;
  env_->Schedule(&test::SleepingBackgroundTask::DoSleepTask, &sleeping_task_low,
                 Env::Priority::LOW);

  options.env = env_;

  DestroyAndReopen(options);

  const int kNumInsertedKeys =
      options.level0_file_num_compaction_trigger *
      (options.max_write_buffer_number - 1) *
      kKeysPerBuffer;

  Random rnd(301);
  std::vector<std::string> keys;
  std::vector<std::string> values;
  for (int k = 0; k < kNumInsertedKeys; ++k) {
    keys.emplace_back(RandomString(&rnd, kKeySize));
    values.emplace_back(RandomString(&rnd, kKvSize - kKeySize));
    ASSERT_OK(Put(Slice(KeyBig(keys[k],kKvSize - kKeySize)), Slice(ValueBig(values[k]))));
    dbfull()->TEST_WaitForFlushMemTable();
  }

  dbfull()->TEST_FlushMemTable(true);
  // Make sure the number of L0 files can trigger compaction.
  ASSERT_GE(NumTableFilesAtLevel(0),
            options.level0_file_num_compaction_trigger);

  auto previous_num_level0_files = NumTableFilesAtLevel(0);

  // Fail the first file creation.
  env_->non_writable_count_ = 1;
  sleeping_task_low.WakeUp();
  sleeping_task_low.WaitUntilDone();

  // Expect compaction to fail here as one file will fail its
  // creation.
  ASSERT_TRUE(!dbfull()->TEST_WaitForCompact().ok());

  // Verify L0 -> L1 compaction does fail.
  ASSERT_EQ(NumTableFilesAtLevel(1), 0);

  // Verify all L0 files are still there.
  ASSERT_EQ(NumTableFilesAtLevel(0), previous_num_level0_files);

  // All key-values must exist after compaction fails.
  for (int k = 0; k < kNumInsertedKeys; ++k) {
    ASSERT_EQ(ValueBig(values[k]), Get(KeyBig(keys[k],kKvSize - kKeySize)));
  }

  env_->non_writable_count_ = 0;

  // Make sure RocksDB will not get into corrupted state.
  Reopen(options);

  // Verify again after reopen.
  for (int k = 0; k < kNumInsertedKeys; ++k) {
    ASSERT_EQ(ValueBig(values[k]), Get(KeyBig(keys[k],kKvSize - kKeySize)));
  }
}

#if 0 // kludge scaf must fix
TEST_P(DBVLogTestWithParam, DeleteMovedFileAfterCompaction) {
  const int value_size = 10*1024;
  // iter 1 -- delete_obsolete_files_period_micros == 0
  for (int iter = 0; iter < 2; ++iter) {
    // This test triggers move compaction and verifies that the file is not
    // deleted when it's part of move compaction
    Options options = CurrentOptions();
    options.env = env_;
    if (iter == 1) {
      options.delete_obsolete_files_period_micros = 0;
    }
    options.create_if_missing = true;
    options.level0_file_num_compaction_trigger =
        2;  // trigger compaction when we have 2 files
    OnFileDeletionListener* listener = new OnFileDeletionListener();
    options.listeners.emplace_back(listener);
    options.max_subcompactions = max_subcompactions_;
#ifdef INDIRECT_VALUE_SUPPORT
    options.allow_trivial_move = true;
#endif
    DestroyAndReopen(options);

    Random rnd(301);
    // Create two 1MB sst files
    for (int i = 0; i < 2; ++i) {
      // Create 1MB sst file
      for (int j = 0; j < 100; ++j) {
        ASSERT_OK(PutBig(i * 50 + j, value_size, RandomString(&rnd, value_size)));
      }
      ASSERT_OK(Flush());
    }
    // this should execute L0->L1
    dbfull()->TEST_WaitForCompact();
    ASSERT_EQ("0,1", FilesPerLevel(0));

    // block compactions
    test::SleepingBackgroundTask sleeping_task;
    env_->Schedule(&test::SleepingBackgroundTask::DoSleepTask, &sleeping_task,
                   Env::Priority::LOW);

    options.max_bytes_for_level_base = 1024 * 1024;  // 1 MB
    Reopen(options);
    std::unique_ptr<Iterator> iterator(db_->NewIterator(ReadOptions()));
    ASSERT_EQ("0,1", FilesPerLevel(0));
    // let compactions go
    sleeping_task.WakeUp();
    sleeping_task.WaitUntilDone();

    // this should execute L1->L2 (move)
    dbfull()->TEST_WaitForCompact();

    ASSERT_EQ("0,0,1", FilesPerLevel(0));

    std::vector<LiveFileMetaData> metadata;
    db_->GetLiveFilesMetaData(&metadata);
    ASSERT_EQ(metadata.size(), 1U);
    auto moved_file_name = metadata[0].name;

    // Create two more 1MB sst files
    for (int i = 0; i < 2; ++i) {
      // Create 1MB sst file
      for (int j = 0; j < 100; ++j) {
        ASSERT_OK(PutBig(i * 50 + j + 100, value_size, RandomString(&rnd, value_size)));
      }
      ASSERT_OK(Flush());
    }
    // this should execute both L0->L1 and L1->L2 (merge with previous file)
    dbfull()->TEST_WaitForCompact();

    ASSERT_EQ("0,0,2", FilesPerLevel(0));

    // iterator is holding the file
    ASSERT_OK(env_->FileExists(dbname_ + moved_file_name));

    listener->SetExpectedFileName(dbname_ + moved_file_name);
    iterator.reset();

    // this file should have been compacted away
    ASSERT_NOK(env_->FileExists(dbname_ + moved_file_name));
    listener->VerifyMatchedCount(1);
  }
}
#endif

TEST_P(DBVLogTestWithParam, CompressLevelCompaction) {
  if (!Zlib_Supported()) {
    return;
  }
  Options options = CurrentOptions();
  options.memtable_factory.reset(
      new SpecialSkipListFactory(KNumKeysByGenerateNewFile - 1));
  options.compaction_style = kCompactionStyleLevel;
  options.write_buffer_size = 110 << 10;  // 110KB
  options.arena_block_size = 4 << 10;
  options.level0_file_num_compaction_trigger = 2;
  options.num_levels = 4;
  options.max_bytes_for_level_base = 400 * 1024;
  options.max_subcompactions = max_subcompactions_;
  // First two levels have no compression, so that a trivial move between
  // them will be allowed. Level 2 has Zlib compression so that a trivial
  // move to level 3 will not be allowed
  options.compression_per_level = {kNoCompression, kNoCompression,
                                   kZlibCompression};
#ifdef INDIRECT_VALUE_SUPPORT
  options.allow_trivial_move = true;
#endif
  int matches = 0, didnt_match = 0, trivial_move = 0, non_trivial = 0;

  rocksdb::SyncPoint::GetInstance()->SetCallBack(
      "Compaction::InputCompressionMatchesOutput:Matches",
      [&](void* arg) { matches++; });
  rocksdb::SyncPoint::GetInstance()->SetCallBack(
      "Compaction::InputCompressionMatchesOutput:DidntMatch",
      [&](void* arg) { didnt_match++; });
  rocksdb::SyncPoint::GetInstance()->SetCallBack(
      "DBImpl::BackgroundCompaction:NonTrivial",
      [&](void* arg) { non_trivial++; });
  rocksdb::SyncPoint::GetInstance()->SetCallBack(
      "DBImpl::BackgroundCompaction:TrivialMove",
      [&](void* arg) { trivial_move++; });
  rocksdb::SyncPoint::GetInstance()->EnableProcessing();

  Reopen(options);

  Random rnd(301);
  int key_idx = 0;

  // First three 110KB files are going to level 0
  // After that, (100K, 200K)
  for (int num = 0; num < 3; num++) {
    GenerateNewFile(&rnd, &key_idx);
  }

  // Another 110KB triggers a compaction to 400K file to fill up level 0
  GenerateNewFile(&rnd, &key_idx);
  ASSERT_EQ(4, GetSstFileCount(dbname_));

  // (1, 4)
  GenerateNewFile(&rnd, &key_idx);
  ASSERT_EQ("1,4", FilesPerLevel(0));

  // (1, 4, 1)
  GenerateNewFile(&rnd, &key_idx);
  ASSERT_EQ("1,4,1", FilesPerLevel(0));

  // (1, 4, 2)
  GenerateNewFile(&rnd, &key_idx);
  ASSERT_EQ("1,4,2", FilesPerLevel(0));

  // (1, 4, 3)
  GenerateNewFile(&rnd, &key_idx);
  ASSERT_EQ("1,4,3", FilesPerLevel(0));

  // (1, 4, 4)
  GenerateNewFile(&rnd, &key_idx);
  ASSERT_EQ("1,4,4", FilesPerLevel(0));

  // (1, 4, 5)
  GenerateNewFile(&rnd, &key_idx);
  ASSERT_EQ("1,4,5", FilesPerLevel(0));

  // (1, 4, 6)
  GenerateNewFile(&rnd, &key_idx);
  ASSERT_EQ("1,4,6", FilesPerLevel(0));

  // (1, 4, 7)
  GenerateNewFile(&rnd, &key_idx);
  ASSERT_EQ("1,4,7", FilesPerLevel(0));

  // (1, 4, 8)
  GenerateNewFile(&rnd, &key_idx);
  ASSERT_EQ("1,4,8", FilesPerLevel(0));

  ASSERT_EQ(matches, 12);
  // Currently, the test relies on the number of calls to
  // InputCompressionMatchesOutput() per compaction.
  const int kCallsToInputCompressionMatch = 2;
  ASSERT_EQ(didnt_match, 8 * kCallsToInputCompressionMatch);
  ASSERT_EQ(trivial_move, 12);
  ASSERT_EQ(non_trivial, 8);

  rocksdb::SyncPoint::GetInstance()->DisableProcessing();

  for (int i = 0; i < key_idx; i++) {
    auto v = Get(Key(i));
    ASSERT_NE(v, "NOT_FOUND");
    ASSERT_TRUE(v.size() == 1 || v.size() == 990);
  }

  Reopen(options);

  for (int i = 0; i < key_idx; i++) {
    auto v = Get(Key(i));
    ASSERT_NE(v, "NOT_FOUND");
    ASSERT_TRUE(v.size() == 1 || v.size() == 990);
  }

  Destroy(options);
}

TEST_F(DBVLogTest, SanitizeCompactionOptionsTest) {
  Options options = CurrentOptions();
  options.max_background_compactions = 5;
  options.soft_pending_compaction_bytes_limit = 0;
  options.hard_pending_compaction_bytes_limit = 100;
  options.create_if_missing = true;
  DestroyAndReopen(options);
  ASSERT_EQ(100, db_->GetOptions().soft_pending_compaction_bytes_limit);

  options.max_background_compactions = 3;
  options.soft_pending_compaction_bytes_limit = 200;
  options.hard_pending_compaction_bytes_limit = 150;
  DestroyAndReopen(options);
  ASSERT_EQ(150, db_->GetOptions().soft_pending_compaction_bytes_limit);
}

// This tests for a bug that could cause two level0 compactions running
// concurrently
// TODO(aekmekji): Make sure that the reason this fails when run with
// max_subcompactions > 1 is not a correctness issue but just inherent to
// running parallel L0-L1 compactions
TEST_F(DBVLogTest, SuggestCompactRangeNoTwoLevel0Compactions) {
  Options options = CurrentOptions();
  options.compaction_style = kCompactionStyleLevel;
  options.write_buffer_size = 110 << 10;
  options.arena_block_size = 4 << 10;
  options.level0_file_num_compaction_trigger = 4;
  options.num_levels = 4;
  options.compression = kNoCompression;
  options.max_bytes_for_level_base = 450 << 10;
  options.target_file_size_base = 98 << 10;
  options.max_write_buffer_number = 2;
  options.max_background_compactions = 2;

  DestroyAndReopen(options);

  // fill up the DB
  Random rnd(301);
  for (int num = 0; num < 10; num++) {
    GenerateNewRandomFile(&rnd);
  }
  db_->CompactRange(CompactRangeOptions(), nullptr, nullptr);

  rocksdb::SyncPoint::GetInstance()->LoadDependency(
      {{"CompactionJob::Run():Start",
        "DBVLogTest::SuggestCompactRangeNoTwoLevel0Compactions:1"},
       {"DBVLogTest::SuggestCompactRangeNoTwoLevel0Compactions:2",
        "CompactionJob::Run():End"}});

  rocksdb::SyncPoint::GetInstance()->EnableProcessing();

  // trigger L0 compaction
  for (int num = 0; num < options.level0_file_num_compaction_trigger + 1;
       num++) {
    GenerateNewRandomFile(&rnd, /* nowait */ true);
    ASSERT_OK(Flush());
  }

  TEST_SYNC_POINT(
      "DBVLogTest::SuggestCompactRangeNoTwoLevel0Compactions:1");

  GenerateNewRandomFile(&rnd, /* nowait */ true);
  dbfull()->TEST_WaitForFlushMemTable();
  ASSERT_OK(experimental::SuggestCompactRange(db_, nullptr, nullptr));
  for (int num = 0; num < options.level0_file_num_compaction_trigger + 1;
       num++) {
    GenerateNewRandomFile(&rnd, /* nowait */ true);
    ASSERT_OK(Flush());
  }

  TEST_SYNC_POINT(
      "DBVLogTest::SuggestCompactRangeNoTwoLevel0Compactions:2");
  dbfull()->TEST_WaitForCompact();
}


TEST_P(DBVLogTestWithParam, ForceBottommostLevelCompaction) {
  int32_t trivial_move = 0;
  int32_t non_trivial_move = 0;
  rocksdb::SyncPoint::GetInstance()->SetCallBack(
      "DBImpl::BackgroundCompaction:TrivialMove",
      [&](void* arg) { trivial_move++; });
  rocksdb::SyncPoint::GetInstance()->SetCallBack(
      "DBImpl::BackgroundCompaction:NonTrivial",
      [&](void* arg) { non_trivial_move++; });
  rocksdb::SyncPoint::GetInstance()->EnableProcessing();

  Options options = CurrentOptions();
  options.write_buffer_size = 100000000;
  options.max_subcompactions = max_subcompactions_;
#ifdef INDIRECT_VALUE_SUPPORT
  options.allow_trivial_move = true;
#endif
  DestroyAndReopen(options);

  int32_t value_size = 10 * 1024;  // 10 KB

  Random rnd(301);
  std::vector<std::string> values;
  // File with keys [ 0 => 99 ]
  for (int i = 0; i < 100; i++) {
    values.push_back(RandomString(&rnd, value_size));
    ASSERT_OK(Put(Key(i), values[i]));
  }
  ASSERT_OK(Flush());

  ASSERT_EQ("1", FilesPerLevel(0));
  // Compaction will do L0=>L1 (trivial move) then move L1 files to L3
  CompactRangeOptions compact_options;
  compact_options.change_level = true;
  compact_options.target_level = 3;
  ASSERT_OK(db_->CompactRange(compact_options, nullptr, nullptr));
  ASSERT_EQ("0,0,0,1", FilesPerLevel(0));
  ASSERT_EQ(trivial_move, 1);
  ASSERT_EQ(non_trivial_move, 0);

  // File with keys [ 100 => 199 ]
  for (int i = 100; i < 200; i++) {
    values.push_back(RandomString(&rnd, value_size));
    ASSERT_OK(Put(Key(i), values[i]));
  }
  ASSERT_OK(Flush());

  ASSERT_EQ("1,0,0,1", FilesPerLevel(0));
  // Compaction will do L0=>L1 L1=>L2 L2=>L3 (3 trivial moves)
  // then compacte the bottommost level L3=>L3 (non trivial move)
  compact_options = CompactRangeOptions();
  compact_options.bottommost_level_compaction =
      BottommostLevelCompaction::kForce;
  ASSERT_OK(db_->CompactRange(compact_options, nullptr, nullptr));
  ASSERT_EQ("0,0,0,1", FilesPerLevel(0));
  ASSERT_EQ(trivial_move, 4);
  ASSERT_EQ(non_trivial_move, 1);

  // File with keys [ 200 => 299 ]
  for (int i = 200; i < 300; i++) {
    values.push_back(RandomString(&rnd, value_size));
    ASSERT_OK(Put(Key(i), values[i]));
  }
  ASSERT_OK(Flush());

  ASSERT_EQ("1,0,0,1", FilesPerLevel(0));
  trivial_move = 0;
  non_trivial_move = 0;
  compact_options = CompactRangeOptions();
  compact_options.bottommost_level_compaction =
      BottommostLevelCompaction::kSkip;
  // Compaction will do L0=>L1 L1=>L2 L2=>L3 (3 trivial moves)
  // and will skip bottommost level compaction
  ASSERT_OK(db_->CompactRange(compact_options, nullptr, nullptr));
  ASSERT_EQ("0,0,0,2", FilesPerLevel(0));
  ASSERT_EQ(trivial_move, 3);
  ASSERT_EQ(non_trivial_move, 0);

  for (int i = 0; i < 300; i++) {
    ASSERT_EQ(Get(Key(i)), values[i]);
  }

  rocksdb::SyncPoint::GetInstance()->DisableProcessing();
}

TEST_P(DBVLogTestWithParam, IntraL0Compaction) {
  Options options = CurrentOptions();
  options.compression = kNoCompression;
  options.level0_file_num_compaction_trigger = 5;
  options.max_background_compactions = 2;
  options.max_subcompactions = max_subcompactions_;
#ifdef INDIRECT_VALUE_SUPPORT
  options.allow_trivial_move = true;
#endif
  DestroyAndReopen(options);

  const size_t kValueSize = 1 << 20;
  Random rnd(301);
  std::string value(RandomString(&rnd, kValueSize));

  rocksdb::SyncPoint::GetInstance()->LoadDependency(
      {{"LevelCompactionPicker::PickCompactionBySize:0",
        "CompactionJob::Run():Start"}});
  rocksdb::SyncPoint::GetInstance()->EnableProcessing();

  // index:   0   1   2   3   4   5   6   7   8   9
  // size:  1MB 1MB 1MB 1MB 1MB 2MB 1MB 1MB 1MB 1MB
  // score:                     1.5 1.3 1.5 2.0 inf
  //
  // Files 0-4 will be included in an L0->L1 compaction.
  //
  // L0->L0 will be triggered since the sync points guarantee compaction to base
  // level is still blocked when files 5-9 trigger another compaction.
  //
  // Files 6-9 are the longest span of available files for which
  // work-per-deleted-file decreases (see "score" row above).
  for (int i = 0; i < 10; ++i) {
    ASSERT_OK(Put(Key(0), ""));  // prevents trivial move
    if (i == 5) {
      ASSERT_OK(Put(KeyBig(i + 1,2*kValueSize), ValueBig(value + value)));
    } else {
      ASSERT_OK(Put(KeyBig(i + 1,kValueSize), ValueBig(value)));
    }
    ASSERT_OK(Flush());
  }
  dbfull()->TEST_WaitForCompact();
  rocksdb::SyncPoint::GetInstance()->DisableProcessing();

  std::vector<std::vector<FileMetaData>> level_to_files;
  dbfull()->TEST_GetFilesMetaData(dbfull()->DefaultColumnFamily(),
                                  &level_to_files);
  ASSERT_GE(level_to_files.size(), 2);  // at least L0 and L1
  // L0 has the 2MB file (not compacted) and 4MB file (output of L0->L0)
  ASSERT_EQ(2, level_to_files[0].size());
  ASSERT_GT(level_to_files[1].size(), 0);
  for (int i = 0; i < 2; ++i) {
    ASSERT_GE(level_to_files[0][i].fd.file_size, 1 << 21);
  }
}

TEST_P(DBVLogTestWithParam, IntraL0CompactionDoesNotObsoleteDeletions) {
  // regression test for issue #2722: L0->L0 compaction can resurrect deleted
  // keys from older L0 files if L1+ files' key-ranges do not include the key.
  Options options = CurrentOptions();
  options.compression = kNoCompression;
  options.level0_file_num_compaction_trigger = 5;
  options.max_background_compactions = 2;
  options.max_subcompactions = max_subcompactions_;
#ifdef INDIRECT_VALUE_SUPPORT
  options.allow_trivial_move = true;
#endif
  DestroyAndReopen(options);

  const size_t kValueSize = 1 << 20;
  Random rnd(301);
  std::string value(RandomString(&rnd, kValueSize));

  rocksdb::SyncPoint::GetInstance()->LoadDependency(
      {{"LevelCompactionPicker::PickCompactionBySize:0",
        "CompactionJob::Run():Start"}});
  rocksdb::SyncPoint::GetInstance()->EnableProcessing();

  // index:   0   1   2   3   4    5    6   7   8   9
  // size:  1MB 1MB 1MB 1MB 1MB  1MB  1MB 1MB 1MB 1MB
  // score:                     1.25 1.33 1.5 2.0 inf
  //
  // Files 0-4 will be included in an L0->L1 compaction.
  //
  // L0->L0 will be triggered since the sync points guarantee compaction to base
  // level is still blocked when files 5-9 trigger another compaction. All files
  // 5-9 are included in the L0->L0 due to work-per-deleted file decreasing.
  //
  // Put a key-value in files 0-4. Delete that key in files 5-9. Verify the
  // L0->L0 preserves the deletion such that the key remains deleted.
  for (int i = 0; i < 10; ++i) {
    // key 0 serves both to prevent trivial move and as the key we want to
    // verify is not resurrected by L0->L0 compaction.
    if (i < 5) {
      ASSERT_OK(Put(Key(0), ""));
    } else {
      ASSERT_OK(Delete(Key(0)));
    }
    ASSERT_OK(Put(KeyBig(i + 1,kValueSize), ValueBig(value)));
    ASSERT_OK(Flush());
  }
  dbfull()->TEST_WaitForCompact();
  rocksdb::SyncPoint::GetInstance()->DisableProcessing();

  std::vector<std::vector<FileMetaData>> level_to_files;
  dbfull()->TEST_GetFilesMetaData(dbfull()->DefaultColumnFamily(),
                                  &level_to_files);
  ASSERT_GE(level_to_files.size(), 2);  // at least L0 and L1
  // L0 has a single output file from L0->L0
  ASSERT_EQ(1, level_to_files[0].size());
  ASSERT_GT(level_to_files[1].size(), 0);
  ASSERT_GE(level_to_files[0][0].fd.file_size, 1 << 22);

  ReadOptions roptions;
  std::string result;
  ASSERT_TRUE(db_->Get(roptions, Key(0), &result).IsNotFound());
}

TEST_F(DBVLogTest, OptimizedDeletionObsoleting) {
  // Deletions can be dropped when compacted to non-last level if they fall
  // outside the lower-level files' key-ranges.
  const int kNumL0Files = 4;
  Options options = CurrentOptions();
  options.level0_file_num_compaction_trigger = kNumL0Files;
  options.statistics = rocksdb::CreateDBStatistics();
#ifdef INDIRECT_VALUE_SUPPORT
  options.allow_trivial_move = true;
#endif
  DestroyAndReopen(options);

  // put key 1 and 3 in separate L1, L2 files.
  // So key 0, 2, and 4+ fall outside these levels' key-ranges.
  for (int level = 2; level >= 1; --level) {
    for (int i = 0; i < 2; ++i) {
      Put(Key(2 * i + 1), "val");
      Flush();
    }
    MoveFilesToLevel(level);
    ASSERT_EQ(2, NumTableFilesAtLevel(level));
  }

  // Delete keys in range [1, 4]. These L0 files will be compacted with L1:
  // - Tombstones for keys 2 and 4 can be dropped early.
  // - Tombstones for keys 1 and 3 must be kept due to L2 files' key-ranges.
  for (int i = 0; i < kNumL0Files; ++i) {
    Put(Key(0), "val");  // sentinel to prevent trivial move
    Delete(Key(i + 1));
    Flush();
  }
  dbfull()->TEST_WaitForCompact();

  for (int i = 0; i < kNumL0Files; ++i) {
    std::string value;
    ASSERT_TRUE(db_->Get(ReadOptions(), Key(i + 1), &value).IsNotFound());
  }
  ASSERT_EQ(2, options.statistics->getTickerCount(
                   COMPACTION_OPTIMIZED_DEL_DROP_OBSOLETE));
  ASSERT_EQ(2,
            options.statistics->getTickerCount(COMPACTION_KEY_DROP_OBSOLETE));
}

TEST_F(DBVLogTest, CompactFilesPendingL0Bug) {
  // https://www.facebook.com/groups/rocksdb.dev/permalink/1389452781153232/
  // CompactFiles() had a bug where it failed to pick a compaction when an L0
  // compaction existed, but marked it as scheduled anyways. It'd never be
  // unmarked as scheduled, so future compactions or DB close could hang.
  const int kNumL0Files = 5;
  Options options = CurrentOptions();
  options.level0_file_num_compaction_trigger = kNumL0Files - 1;
  options.max_background_compactions = 2;
  DestroyAndReopen(options);

  rocksdb::SyncPoint::GetInstance()->LoadDependency(
      {{"LevelCompactionPicker::PickCompaction:Return",
        "DBVLogTest::CompactFilesPendingL0Bug:Picked"},
       {"DBVLogTest::CompactFilesPendingL0Bug:ManualCompacted",
        "DBImpl::BackgroundCompaction:NonTrivial:AfterRun"}});
  rocksdb::SyncPoint::GetInstance()->EnableProcessing();

  auto schedule_multi_compaction_token =
      dbfull()->TEST_write_controler().GetCompactionPressureToken();

  // Files 0-3 will be included in an L0->L1 compaction.
  //
  // File 4 will be included in a call to CompactFiles() while the first
  // compaction is running.
  for (int i = 0; i < kNumL0Files - 1; ++i) {
    ASSERT_OK(Put(Key(0), "val"));  // sentinel to prevent trivial move
    ASSERT_OK(Put(Key(i + 1), "val"));
    ASSERT_OK(Flush());
  }
  TEST_SYNC_POINT("DBVLogTest::CompactFilesPendingL0Bug:Picked");
  // file 4 flushed after 0-3 picked
  ASSERT_OK(Put(Key(kNumL0Files), "val"));
  ASSERT_OK(Flush());

  // previously DB close would hang forever as this situation caused scheduled
  // compactions count to never decrement to zero.
  ColumnFamilyMetaData cf_meta;
  dbfull()->GetColumnFamilyMetaData(dbfull()->DefaultColumnFamily(), &cf_meta);
  ASSERT_EQ(kNumL0Files, cf_meta.levels[0].files.size());
  std::vector<std::string> input_filenames;
  input_filenames.push_back(cf_meta.levels[0].files.front().name);
  ASSERT_OK(dbfull()
                  ->CompactFiles(CompactionOptions(), input_filenames,
                                 0 /* output_level */));
  TEST_SYNC_POINT("DBVLogTest::CompactFilesPendingL0Bug:ManualCompacted");
  rocksdb::SyncPoint::GetInstance()->DisableProcessing();
}

TEST_F(DBVLogTest, CompactFilesOverlapInL0Bug) {
  // Regression test for bug of not pulling in L0 files that overlap the user-
  // specified input files in time- and key-ranges.
  Put(Key(0), "old_val");
  Flush();
  Put(Key(0), "new_val");
  Flush();

  ColumnFamilyMetaData cf_meta;
  dbfull()->GetColumnFamilyMetaData(dbfull()->DefaultColumnFamily(), &cf_meta);
  ASSERT_GE(cf_meta.levels.size(), 2);
  ASSERT_EQ(2, cf_meta.levels[0].files.size());

  // Compacting {new L0 file, L1 file} should pull in the old L0 file since it
  // overlaps in key-range and time-range.
  std::vector<std::string> input_filenames;
  input_filenames.push_back(cf_meta.levels[0].files.front().name);
  ASSERT_OK(dbfull()->CompactFiles(CompactionOptions(), input_filenames,
                                   1 /* output_level */));
  ASSERT_EQ("new_val", Get(Key(0)));
}

INSTANTIATE_TEST_CASE_P(DBVLogTestWithParam, DBVLogTestWithParam,
                        ::testing::Values(std::make_tuple(1, true),
                                          std::make_tuple(1, false),
                                          std::make_tuple(4, true),
                                          std::make_tuple(4, false)));

TEST_P(DBCompactionDirectIOTest, DirectIO) {
  Options options = CurrentOptions();
  Destroy(options);
  options.create_if_missing = true;
  options.disable_auto_compactions = true;
  options.use_direct_io_for_flush_and_compaction = GetParam();
  options.env = new MockEnv(Env::Default());
  Reopen(options);
  bool readahead = false;
  SyncPoint::GetInstance()->SetCallBack(
      "TableCache::NewIterator:for_compaction", [&](void* arg) {
        bool* use_direct_reads = static_cast<bool*>(arg);
        ASSERT_EQ(*use_direct_reads,
                  options.use_direct_io_for_flush_and_compaction);
      });
  SyncPoint::GetInstance()->SetCallBack(
      "CompactionJob::OpenCompactionOutputFile", [&](void* arg) {
        bool* use_direct_writes = static_cast<bool*>(arg);
        ASSERT_EQ(*use_direct_writes,
                  options.use_direct_io_for_flush_and_compaction);
      });
  if (options.use_direct_io_for_flush_and_compaction) {
    SyncPoint::GetInstance()->SetCallBack(
        "SanitizeOptions:direct_io", [&](void* arg) {
          readahead = true;
        });
  }
  SyncPoint::GetInstance()->EnableProcessing();
  CreateAndReopenWithCF({"pikachu"}, options);
  MakeTables(3, "p", "q", 1);
  ASSERT_EQ("1,1,1", FilesPerLevel(1));
  Compact(1, "p1", "p9");
  ASSERT_FALSE(readahead ^ options.use_direct_io_for_flush_and_compaction);
  ASSERT_EQ("0,0,1", FilesPerLevel(1));
  Destroy(options);
  delete options.env;
}

INSTANTIATE_TEST_CASE_P(DBCompactionDirectIOTest, DBCompactionDirectIOTest,
                        testing::Bool());

class CompactionPriTest : public DBTestBase,
                          public testing::WithParamInterface<uint32_t> {
 public:
  CompactionPriTest() : DBTestBase("/compaction_pri_test") {
    compaction_pri_ = GetParam();
  }

  // Required if inheriting from testing::WithParamInterface<>
  static void SetUpTestCase() {}
  static void TearDownTestCase() {}

  uint32_t compaction_pri_;
};

TEST_P(CompactionPriTest, Test) {
  Options options = CurrentOptions();
  options.write_buffer_size = 16 * 1024;
  options.compaction_pri = static_cast<CompactionPri>(compaction_pri_);
  options.hard_pending_compaction_bytes_limit = 256 * 1024;
  options.max_bytes_for_level_base = 64 * 1024;
  options.max_bytes_for_level_multiplier = 4;
  options.compression = kNoCompression;

  DestroyAndReopen(options);

  Random rnd(301);
  const int kNKeys = 5000;
  int keys[kNKeys];
  for (int i = 0; i < kNKeys; i++) {
    keys[i] = i;
  }
  std::random_shuffle(std::begin(keys), std::end(keys));

  for (int i = 0; i < kNKeys; i++) {
    ASSERT_OK(Put(KeyBig(keys[i],102), ValueBig(RandomString(&rnd, 102))));
  }

  dbfull()->TEST_WaitForCompact();
  for (int i = 0; i < kNKeys; i++) {
    ASSERT_NE("NOT_FOUND", Get(KeyBig(i,102)));
  }
}
INSTANTIATE_TEST_CASE_P(
    CompactionPriTest, CompactionPriTest,
    ::testing::Values(CompactionPri::kByCompensatedSize,
                      CompactionPri::kOldestLargestSeqFirst,
                      CompactionPri::kOldestSmallestSeqFirst,
                      CompactionPri::kMinOverlappingRatio));

#endif // !defined(ROCKSDB_LITE)
}  // namespace rocksdb

#endif

int main(int argc, char** argv) {
#if !defined(ROCKSDB_LITE)
  rocksdb::port::InstallStackTraceHandler();
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
#else
  return 0;
#endif
}
