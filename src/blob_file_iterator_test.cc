#include "blob_file_iterator.h"

#include <cinttypes>

#include "blob_file_builder.h"
#include "blob_file_cache.h"
#include "blob_file_reader.h"
#include "file/filename.h"
#include "test_util/testharness.h"
#include "util/random.h"

namespace rocksdb {
namespace titandb {

class BlobFileIteratorTest : public testing::Test {
 public:
  Env* env_{Env::Default()};
  TitanOptions titan_options_;
  EnvOptions env_options_;
  std::string dirname_;
  std::string file_name_;
  uint64_t file_number_;
  std::unique_ptr<BlobFileBuilder> builder_;
  std::unique_ptr<WritableFileWriter> writable_file_;
  std::unique_ptr<BlobFileIterator> blob_file_iterator_;
  std::unique_ptr<RandomAccessFileReader> readable_file_;

  BlobFileIteratorTest() : dirname_(test::TmpDir(env_)) {
    titan_options_.dirname = dirname_;
    file_number_ = Random::GetTLSInstance()->Next();
    file_name_ = BlobFileName(dirname_, file_number_);
  }

  ~BlobFileIteratorTest() {
    env_->DeleteFile(file_name_);
    env_->DeleteDir(dirname_);
  }

  std::string GenKey(uint64_t i) {
    char buf[64];
    snprintf(buf, sizeof(buf), "k-%08" PRIu64, i);
    return buf;
  }

  std::string GenValue(uint64_t k) {
    if (k % 2 == 0) {
      return std::string(titan_options_.min_blob_size - 1, 'v');
    } else {
      return std::string(titan_options_.min_blob_size + 1, 'v');
    }
  }

  void NewBuilder() {
    TitanDBOptions db_options(titan_options_);
    TitanCFOptions cf_options(titan_options_);
    BlobFileCache cache(db_options, cf_options, {NewLRUCache(128)}, nullptr);

    {
      std::unique_ptr<WritableFile> f;
      ASSERT_OK(env_->NewWritableFile(file_name_, &f, env_options_));
      writable_file_.reset(
          new WritableFileWriter(std::move(f), file_name_, env_options_));
    }
    builder_.reset(
        new BlobFileBuilder(db_options, cf_options, writable_file_.get()));
  }

  BlobIndices AddKeyValue(const std::string& key, const std::string& value) {
    BlobRecord record;
    record.key = key;
    record.value = value;
    std::unique_ptr<BlobIndex> idx(new BlobIndex);
    BlobIndices key_indices = builder_->Add(record, std::move(idx));
    // ASSERT_OK(builder_->status());
    return key_indices;
  }

  void FinishBuilder() {
    ASSERT_OK(builder_->Finish());
    ASSERT_OK(builder_->status());
  }

  void NewBlobFileIterator() {
    uint64_t file_size = 0;
    ASSERT_OK(env_->GetFileSize(file_name_, &file_size));
    NewBlobFileReader(file_number_, 0, titan_options_, env_options_, env_,
                      &readable_file_);
    blob_file_iterator_.reset(new BlobFileIterator{
        std::move(readable_file_), file_number_, file_size, TitanCFOptions()});
  }

  void TestBlobFileIterator() {
    NewBuilder();

    const int n = 1000;
    BlobIndices key_indices;
    for (int i = 0; i < n; i++) {
      BlobIndices cur_key_indices = AddKeyValue(GenKey(i), GenValue(i));
      if (!cur_key_indices.empty()) {
        key_indices.insert(key_indices.end(),
                           std::make_move_iterator(cur_key_indices.begin()),
                           std::make_move_iterator(cur_key_indices.end()));
      }
    }

    FinishBuilder();

    NewBlobFileIterator();

    blob_file_iterator_->SeekToFirst();
    ASSERT_EQ(key_indices.size(), n);
    for (int i = 0; i < n; blob_file_iterator_->Next(), i++) {
      ASSERT_OK(blob_file_iterator_->status());
      ASSERT_EQ(blob_file_iterator_->Valid(), true);
      ASSERT_EQ(GenKey(i), blob_file_iterator_->key());
      ASSERT_EQ(GenValue(i), blob_file_iterator_->value());
      BlobIndex blob_index = blob_file_iterator_->GetBlobIndex();
      ASSERT_EQ(key_indices[i].second->blob_handle, blob_index.blob_handle);
    }
  }
};

TEST_F(BlobFileIteratorTest, Basic) {
  TitanOptions options;
  TestBlobFileIterator();
}

TEST_F(BlobFileIteratorTest, IterateForPrev) {
  NewBuilder();
  const int n = 1000;
  BlobIndices key_indices;
  for (int i = 0; i < n; i++) {
    BlobIndices cur_key_indices = AddKeyValue(GenKey(i), GenValue(i));
    if (!cur_key_indices.empty()) {
      key_indices.insert(key_indices.end(),
                         std::make_move_iterator(cur_key_indices.begin()),
                         std::make_move_iterator(cur_key_indices.end()));
    }
  }

  FinishBuilder();

  NewBlobFileIterator();

  int i = n / 2;
  ASSERT_EQ(key_indices.size(), n);
  blob_file_iterator_->IterateForPrev(
      key_indices[i].second->blob_handle.offset);
  ASSERT_OK(blob_file_iterator_->status());
  for (blob_file_iterator_->Next(); i < n; i++, blob_file_iterator_->Next()) {
    ASSERT_OK(blob_file_iterator_->status());
    ASSERT_EQ(blob_file_iterator_->Valid(), true);
    BlobIndex blob_index;
    blob_index = blob_file_iterator_->GetBlobIndex();
    ASSERT_EQ(key_indices[i].second->blob_handle, blob_index.blob_handle);
    ASSERT_EQ(GenKey(i), blob_file_iterator_->key());
    ASSERT_EQ(GenValue(i), blob_file_iterator_->value());
  }

  auto idx = Random::GetTLSInstance()->Uniform(n);
  blob_file_iterator_->IterateForPrev(
      key_indices[idx].second->blob_handle.offset);
  ASSERT_OK(blob_file_iterator_->status());
  blob_file_iterator_->Next();
  ASSERT_OK(blob_file_iterator_->status());
  ASSERT_TRUE(blob_file_iterator_->Valid());
  BlobIndex blob_index;
  blob_index = blob_file_iterator_->GetBlobIndex();
  ASSERT_EQ(key_indices[idx].second->blob_handle, blob_index.blob_handle);

  while ((idx = Random::GetTLSInstance()->Uniform(n)) == 0)
    ;
  blob_file_iterator_->IterateForPrev(
      key_indices[idx].second->blob_handle.offset - kRecordHeaderSize - 1);
  ASSERT_OK(blob_file_iterator_->status());
  blob_file_iterator_->Next();
  ASSERT_OK(blob_file_iterator_->status());
  ASSERT_TRUE(blob_file_iterator_->Valid());
  blob_index = blob_file_iterator_->GetBlobIndex();
  ASSERT_EQ(key_indices[idx - 1].second->blob_handle, blob_index.blob_handle);

  idx = Random::GetTLSInstance()->Uniform(n);
  blob_file_iterator_->IterateForPrev(
      key_indices[idx].second->blob_handle.offset + 1);
  ASSERT_OK(blob_file_iterator_->status());
  blob_file_iterator_->Next();
  ASSERT_OK(blob_file_iterator_->status());
  ASSERT_TRUE(blob_file_iterator_->Valid());
  blob_index = blob_file_iterator_->GetBlobIndex();
  ASSERT_EQ(key_indices[idx].second->blob_handle, blob_index.blob_handle);
}

TEST_F(BlobFileIteratorTest, MergeIterator) {
  const int kMaxKeyNum = 1000;
  BlobIndices key_indices;
  std::vector<std::unique_ptr<BlobFileIterator>> iters;
  NewBuilder();
  for (int i = 1; i < kMaxKeyNum; i++) {
    BlobIndices cur_key_indices = AddKeyValue(GenKey(i), GenValue(i));
    if (!cur_key_indices.empty()) {
      key_indices.insert(key_indices.end(),
                         std::make_move_iterator(cur_key_indices.begin()),
                         std::make_move_iterator(cur_key_indices.end()));
    }
    if (i % 100 == 0) {
      // FIXME: refactor finish
      FinishBuilder();
      uint64_t file_size = 0;
      ASSERT_OK(env_->GetFileSize(file_name_, &file_size));
      NewBlobFileReader(file_number_, 0, titan_options_, env_options_, env_,
                        &readable_file_);
      iters.emplace_back(std::unique_ptr<BlobFileIterator>(
          new BlobFileIterator{std::move(readable_file_), file_number_,
                               file_size, TitanCFOptions()}));
      file_number_ = Random::GetTLSInstance()->Next();
      file_name_ = BlobFileName(dirname_, file_number_);
      NewBuilder();
    }
  }

  FinishBuilder();
  uint64_t file_size = 0;
  ASSERT_OK(env_->GetFileSize(file_name_, &file_size));
  NewBlobFileReader(file_number_, 0, titan_options_, env_options_, env_,
                    &readable_file_);
  iters.emplace_back(std::unique_ptr<BlobFileIterator>(new BlobFileIterator{
      std::move(readable_file_), file_number_, file_size, TitanCFOptions()}));
  BlobFileMergeIterator iter(std::move(iters), titan_options_.comparator);

  iter.SeekToFirst();
  int i = 1;
  for (; iter.Valid(); i++, iter.Next()) {
    ASSERT_OK(iter.status());
    ASSERT_TRUE(iter.Valid());
    ASSERT_EQ(iter.key(), GenKey(i));
    ASSERT_EQ(iter.value(), GenValue(i));
    // FIXME: broke
    ASSERT_EQ(iter.GetBlobIndex().blob_handle,
              key_indices[i].second->blob_handle);
  }
  ASSERT_EQ(i, kMaxKeyNum);
}

}  // namespace titandb
}  // namespace rocksdb

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
