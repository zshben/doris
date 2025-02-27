// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#include "olap/rowset/segment_v2/inverted_index_writer.h"

#include <CLucene.h> // IWYU pragma: keep
#include <CLucene/analysis/LanguageBasedAnalyzer.h>
#include <CLucene/util/bkd/bkd_writer.h>
#include <glog/logging.h>

#include <limits>
#include <memory>
#include <ostream>
#include <roaring/roaring.hh>
#include <vector>

#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wshadow-field"
#endif
#include "CLucene/analysis/standard95/StandardAnalyzer.h"
#ifdef __clang__
#pragma clang diagnostic pop
#endif
#include "common/config.h"
#include "olap/field.h"
#include "olap/inverted_index_parser.h"
#include "olap/key_coder.h"
#include "olap/olap_common.h"
#include "olap/rowset/segment_v2/common.h"
#include "olap/rowset/segment_v2/inverted_index/char_filter/char_filter_factory.h"
#include "olap/rowset/segment_v2/inverted_index_cache.h"
#include "olap/rowset/segment_v2/inverted_index_compound_directory.h"
#include "olap/rowset/segment_v2/inverted_index_desc.h"
#include "olap/rowset/segment_v2/inverted_index_reader.h"
#include "olap/tablet_schema.h"
#include "olap/types.h"
#include "runtime/collection_value.h"
#include "util/debug_points.h"
#include "util/faststring.h"
#include "util/slice.h"
#include "util/string_util.h"

#define FINALLY_CLOSE_OUTPUT(x)       \
    try {                             \
        if (x != nullptr) x->close(); \
    } catch (...) {                   \
    }
namespace doris::segment_v2 {
const int32_t MAX_FIELD_LEN = 0x7FFFFFFFL;
const int32_t MERGE_FACTOR = 100000000;
const int32_t MAX_LEAF_COUNT = 1024;
const float MAXMBSortInHeap = 512.0 * 8;
const int DIMS = 1;

template <FieldType field_type>
class InvertedIndexColumnWriterImpl : public InvertedIndexColumnWriter {
public:
    using CppType = typename CppTypeTraits<field_type>::CppType;

    explicit InvertedIndexColumnWriterImpl(const std::string& field_name,
                                           const std::string& segment_file_name,
                                           const std::string& dir, const io::FileSystemSPtr& fs,
                                           const TabletIndex* index_meta)
            : _segment_file_name(segment_file_name),
              _directory(dir),
              _fs(fs),
              _index_meta(index_meta) {
        _parser_type = get_inverted_index_parser_type_from_string(
                get_parser_string_from_properties(_index_meta->properties()));
        _value_key_coder = get_key_coder(field_type);
        _field_name = std::wstring(field_name.begin(), field_name.end());
    }

    ~InvertedIndexColumnWriterImpl() override = default;

    Status init() override {
        try {
            if constexpr (field_is_slice_type(field_type)) {
                return init_fulltext_index();
            } else if constexpr (field_is_numeric_type(field_type)) {
                return init_bkd_index();
            }
            return Status::Error<doris::ErrorCode::INVERTED_INDEX_NOT_SUPPORTED>(
                    "Field type not supported");
        } catch (const CLuceneError& e) {
            LOG(WARNING) << "Inverted index writer init error occurred: " << e.what();
            return Status::Error<doris::ErrorCode::INVERTED_INDEX_CLUCENE_ERROR>(
                    "Inverted index writer init error occurred");
        }
    }

    void close() {
        if (_index_writer) {
            _index_writer->close();
            if (config::enable_write_index_searcher_cache) {
                // open index searcher into cache
                auto mem_tracker =
                        std::make_unique<MemTracker>("InvertedIndexSearcherCacheWithRead");
                io::Path index_dir(_directory);
                auto index_file_name = InvertedIndexDescriptor::get_index_file_name(
                        _segment_file_name, _index_meta->index_id(),
                        _index_meta->get_index_suffix());
                IndexSearcherPtr searcher;
                auto st = InvertedIndexReader::create_index_searcher(
                        &searcher, _fs, index_dir, index_file_name, mem_tracker.get(),
                        InvertedIndexReaderType::FULLTEXT);
                if (UNLIKELY(!st.ok())) {
                    LOG(ERROR) << "insert inverted index searcher cache error:" << st;
                    return;
                }
                auto* cache_value = new InvertedIndexSearcherCache::CacheValue(
                        std::move(searcher), mem_tracker->consumption(), UnixMillis());
                InvertedIndexSearcherCache::CacheKey searcher_cache_key(
                        (index_dir / index_file_name).native());
                InvertedIndexSearcherCache::instance()->insert(searcher_cache_key, cache_value);
            }
        }
    }

    void close_on_error() override {
        try {
            if (_index_writer) {
                _index_writer->close();
            }
            if (_dir) {
                _dir->deleteDirectory();
                io::Path cfs_path(_dir->getCfsDirName());
                auto idx_path = cfs_path.parent_path();
                std::string idx_name = std::string(cfs_path.stem().c_str()) +
                                       DorisCompoundDirectory::COMPOUND_FILE_EXTENSION;
                _dir->deleteFile(idx_name.c_str());
            }
        } catch (CLuceneError& e) {
            LOG(ERROR) << "InvertedIndexWriter close_on_error failure: " << e.what();
        }
    }

    Status init_bkd_index() {
        size_t value_length = sizeof(CppType);
        // NOTE: initialize with 0, set to max_row_id when finished.
        int32_t max_doc = 0;
        int32_t total_point_count = std::numeric_limits<std::int32_t>::max();
        _bkd_writer = std::make_shared<lucene::util::bkd::bkd_writer>(
                max_doc, DIMS, DIMS, value_length, MAX_LEAF_COUNT, MAXMBSortInHeap,
                total_point_count, true, config::max_depth_in_bkd_tree);
        return create_index_directory(_dir);
    }

    std::unique_ptr<lucene::analysis::Analyzer> create_chinese_analyzer() {
        auto chinese_analyzer = std::make_unique<lucene::analysis::LanguageBasedAnalyzer>();
        chinese_analyzer->setLanguage(L"chinese");
        chinese_analyzer->initDict(config::inverted_index_dict_path);

        auto mode = get_parser_mode_string_from_properties(_index_meta->properties());
        if (mode == INVERTED_INDEX_PARSER_FINE_GRANULARITY) {
            chinese_analyzer->setMode(lucene::analysis::AnalyzerMode::All);
        } else {
            chinese_analyzer->setMode(lucene::analysis::AnalyzerMode::Default);
        }

        return chinese_analyzer;
    }

    Status create_char_string_reader(std::unique_ptr<lucene::util::Reader>& string_reader) {
        CharFilterMap char_filter_map =
                get_parser_char_filter_map_from_properties(_index_meta->properties());
        if (!char_filter_map.empty()) {
            string_reader = std::unique_ptr<lucene::util::Reader>(CharFilterFactory::create(
                    char_filter_map[INVERTED_INDEX_PARSER_CHAR_FILTER_TYPE],
                    new lucene::util::SStringReader<char>(),
                    char_filter_map[INVERTED_INDEX_PARSER_CHAR_FILTER_PATTERN],
                    char_filter_map[INVERTED_INDEX_PARSER_CHAR_FILTER_REPLACEMENT]));
        } else {
            string_reader = std::make_unique<lucene::util::SStringReader<char>>();
        }
        return Status::OK();
    }

    Status create_index_directory(std::unique_ptr<DorisCompoundDirectory>& dir) {
        bool use_compound_file_writer = true;
        bool can_use_ram_dir = true;
        auto index_path = InvertedIndexDescriptor::get_temporary_index_path(
                _directory + "/" + _segment_file_name, _index_meta->index_id(),
                _index_meta->get_index_suffix());

        bool exists = false;
        auto st = _fs->exists(index_path.c_str(), &exists);
        if (!st.ok()) {
            LOG(ERROR) << "index_path: exists error:" << st;
            return st;
        }
        if (exists) {
            LOG(ERROR) << "try to init a directory:" << index_path << " already exists";
            return Status::InternalError("init_fulltext_index directory already exists");
        }

        dir = std::unique_ptr<DorisCompoundDirectory>(DorisCompoundDirectoryFactory::getDirectory(
                _fs, index_path.c_str(), use_compound_file_writer, can_use_ram_dir));
        return Status::OK();
    }

    Status create_index_writer(std::unique_ptr<lucene::index::IndexWriter>& index_writer) {
        bool create_index = true;
        bool close_dir_on_shutdown = true;
        index_writer = std::make_unique<lucene::index::IndexWriter>(
                _dir.get(), _analyzer.get(), create_index, close_dir_on_shutdown);
        index_writer->setRAMBufferSizeMB(config::inverted_index_ram_buffer_size);
        _index_writer->setMaxBufferedDocs(config::inverted_index_max_buffered_docs);
        index_writer->setMaxFieldLength(MAX_FIELD_LEN);
        index_writer->setMergeFactor(MERGE_FACTOR);
        index_writer->setUseCompoundFile(false);

        return Status::OK();
    }

    Status create_field(lucene::document::Field** field) {
        int field_config = int(lucene::document::Field::STORE_NO) |
                           int(lucene::document::Field::INDEX_NONORMS);
        field_config |= (_parser_type == InvertedIndexParserType::PARSER_NONE)
                                ? int(lucene::document::Field::INDEX_UNTOKENIZED)
                                : int(lucene::document::Field::INDEX_TOKENIZED);
        *field = new lucene::document::Field(_field_name.c_str(), field_config);
        (*field)->setOmitTermFreqAndPositions(
                get_parser_phrase_support_string_from_properties(_index_meta->properties()) ==
                                INVERTED_INDEX_PARSER_PHRASE_SUPPORT_YES
                        ? false
                        : true);
        return Status::OK();
    }

    Status create_analyzer(std::unique_ptr<lucene::analysis::Analyzer>& analyzer) {
        switch (_parser_type) {
        case InvertedIndexParserType::PARSER_STANDARD:
        case InvertedIndexParserType::PARSER_UNICODE:
            analyzer = std::make_unique<lucene::analysis::standard95::StandardAnalyzer>();
            break;
        case InvertedIndexParserType::PARSER_ENGLISH:
            analyzer = std::make_unique<lucene::analysis::SimpleAnalyzer<char>>();
            break;
        case InvertedIndexParserType::PARSER_CHINESE:
            analyzer = create_chinese_analyzer();
            break;
        default:
            analyzer = std::make_unique<lucene::analysis::SimpleAnalyzer<char>>();
            break;
        }
        setup_analyzer_lowercase(analyzer);
        return Status::OK();
    }

    void setup_analyzer_lowercase(std::unique_ptr<lucene::analysis::Analyzer>& analyzer) {
        auto lowercase = get_parser_lowercase_from_properties(_index_meta->properties());
        if (lowercase == "true") {
            analyzer->set_lowercase(true);
        } else if (lowercase == "false") {
            analyzer->set_lowercase(false);
        }
    }

    Status init_fulltext_index() {
        RETURN_IF_ERROR(create_index_directory(_dir));
        RETURN_IF_ERROR(create_char_string_reader(_char_string_reader));
        RETURN_IF_ERROR(create_analyzer(_analyzer));
        RETURN_IF_ERROR(create_index_writer(_index_writer));
        RETURN_IF_ERROR(create_field(&_field));
        _doc = std::make_unique<lucene::document::Document>();
        _doc->add(*_field);
        return Status::OK();
    }

    Status add_document() {
        try {
            _index_writer->addDocument(_doc.get());
        } catch (const CLuceneError& e) {
            close_on_error();
            _dir->deleteDirectory();
            return Status::Error<ErrorCode::INVERTED_INDEX_CLUCENE_ERROR>(
                    "CLuceneError add_document: {}", e.what());
        }
        return Status::OK();
    }

    Status add_null_document() {
        try {
            _index_writer->addNullDocument(_doc.get());
        } catch (const CLuceneError& e) {
            close_on_error();
            _dir->deleteDirectory();
            return Status::Error<ErrorCode::INVERTED_INDEX_CLUCENE_ERROR>(
                    "CLuceneError add_null_document: {}", e.what());
        }
        return Status::OK();
    }

    Status add_nulls(uint32_t count) override {
        _null_bitmap.addRange(_rid, _rid + count);
        _rid += count;
        if constexpr (field_is_slice_type(field_type)) {
            if (_field == nullptr || _index_writer == nullptr) {
                LOG(ERROR) << "field or index writer is null in inverted index writer.";
                return Status::InternalError(
                        "field or index writer is null in inverted index writer");
            }

            for (int i = 0; i < count; ++i) {
                RETURN_IF_ERROR(add_null_document());
            }
        }
        return Status::OK();
    }

    void new_fulltext_field(const char* field_value_data, size_t field_value_size) {
        if (_parser_type == InvertedIndexParserType::PARSER_ENGLISH ||
            _parser_type == InvertedIndexParserType::PARSER_CHINESE ||
            _parser_type == InvertedIndexParserType::PARSER_UNICODE ||
            _parser_type == InvertedIndexParserType::PARSER_STANDARD) {
            new_char_token_stream(field_value_data, field_value_size, _field);
        } else {
            new_field_char_value(field_value_data, field_value_size, _field);
        }
    }

    void new_char_token_stream(const char* s, size_t len, lucene::document::Field* field) {
        _char_string_reader->init(s, len, false);
        auto stream = _analyzer->reusableTokenStream(field->name(), _char_string_reader.get());
        field->setValue(stream);
    }

    void new_field_value(const char* s, size_t len, lucene::document::Field* field) {
        auto field_value = lucene::util::Misc::_charToWide(s, len);
        field->setValue(field_value, false);
        // setValue did not duplicate value, so we don't have to delete
        //_CLDELETE_ARRAY(field_value)
    }

    void new_field_char_value(const char* s, size_t len, lucene::document::Field* field) {
        field->setValue((char*)s, len);
    }

    Status add_values(const std::string fn, const void* values, size_t count) override {
        if constexpr (field_is_slice_type(field_type)) {
            if (_field == nullptr || _index_writer == nullptr) {
                LOG(ERROR) << "field or index writer is null in inverted index writer.";
                return Status::InternalError(
                        "field or index writer is null in inverted index writer");
            }
            auto* v = (Slice*)values;
            auto ignore_above_value =
                    get_parser_ignore_above_value_from_properties(_index_meta->properties());
            auto ignore_above = std::stoi(ignore_above_value);
            for (int i = 0; i < count; ++i) {
                // only ignore_above UNTOKENIZED strings and empty strings not tokenized
                if ((_parser_type == InvertedIndexParserType::PARSER_NONE &&
                     v->get_size() > ignore_above) ||
                    (_parser_type != InvertedIndexParserType::PARSER_NONE && v->empty())) {
                    RETURN_IF_ERROR(add_null_document());
                } else {
                    new_fulltext_field(v->get_data(), v->get_size());
                    RETURN_IF_ERROR(add_document());
                }
                ++v;
                _rid++;
            }
        } else if constexpr (field_is_numeric_type(field_type)) {
            add_numeric_values(values, count);
        }
        return Status::OK();
    }

    Status add_array_values(size_t field_size, const void* value_ptr, const uint8_t* null_map,
                            const uint8_t* offsets_ptr, size_t count) override {
        if (count == 0) {
            // no values to add inverted index
            return Status::OK();
        }
        const auto* offsets = reinterpret_cast<const uint64_t*>(offsets_ptr);
        if constexpr (field_is_slice_type(field_type)) {
            if (_field == nullptr || _index_writer == nullptr) {
                LOG(ERROR) << "field or index writer is null in inverted index writer.";
                return Status::InternalError(
                        "field or index writer is null in inverted index writer");
            }
            auto ignore_above_value =
                    get_parser_ignore_above_value_from_properties(_index_meta->properties());
            auto ignore_above = std::stoi(ignore_above_value);
            for (int i = 0; i < count; ++i) {
                // offsets[i+1] is now row element count
                std::vector<std::string> strings;
                // [0, 3, 6]
                // [10,20,30] [20,30,40], [30,40,50]
                auto start_off = offsets[i];
                auto end_off = offsets[i + 1];
                for (auto j = start_off; j < end_off; ++j) {
                    if (null_map[j] == 1) {
                        continue;
                    }
                    auto* v = (Slice*)((const uint8_t*)value_ptr + j * field_size);
                    strings.emplace_back(v->get_data(), v->get_size());
                }

                auto value = join(strings, " ");
                // only ignore_above UNTOKENIZED strings and empty strings not tokenized
                if ((_parser_type == InvertedIndexParserType::PARSER_NONE &&
                     value.length() > ignore_above) ||
                    (_parser_type != InvertedIndexParserType::PARSER_NONE && value.empty())) {
                    RETURN_IF_ERROR(add_null_document());
                } else {
                    new_fulltext_field(value.c_str(), value.length());
                    RETURN_IF_ERROR(add_document());
                }
                _rid++;
            }
        } else if constexpr (field_is_numeric_type(field_type)) {
            for (int i = 0; i < count; ++i) {
                auto start_off = offsets[i];
                auto end_off = offsets[i + 1];
                for (size_t j = start_off; j < end_off; ++j) {
                    if (null_map[j] == 1) {
                        continue;
                    }
                    const CppType* p = &reinterpret_cast<const CppType*>(value_ptr)[j];
                    std::string new_value;
                    size_t value_length = sizeof(CppType);

                    _value_key_coder->full_encode_ascending(p, &new_value);
                    _bkd_writer->add((const uint8_t*)new_value.c_str(), value_length, _rid);
                }
                _row_ids_seen_for_bkd++;
                _rid++;
            }
        }
        return Status::OK();
    }
    Status add_array_values(size_t field_size, const CollectionValue* values,
                            size_t count) override {
        if constexpr (field_is_slice_type(field_type)) {
            if (_field == nullptr || _index_writer == nullptr) {
                LOG(ERROR) << "field or index writer is null in inverted index writer.";
                return Status::InternalError(
                        "field or index writer is null in inverted index writer");
            }
            for (int i = 0; i < count; ++i) {
                auto* item_data_ptr = const_cast<CollectionValue*>(values)->mutable_data();
                std::vector<std::string> strings;

                for (size_t j = 0; j < values->length(); ++j) {
                    auto* v = (Slice*)item_data_ptr;

                    if (!values->is_null_at(j)) {
                        strings.emplace_back(v->get_data(), v->get_size());
                    }
                    item_data_ptr = (uint8_t*)item_data_ptr + field_size;
                }
                auto value = join(strings, " ");
                new_fulltext_field(value.c_str(), value.length());
                _rid++;
                RETURN_IF_ERROR(add_document());
                values++;
            }
        } else if constexpr (field_is_numeric_type(field_type)) {
            for (int i = 0; i < count; ++i) {
                auto* item_data_ptr = const_cast<CollectionValue*>(values)->mutable_data();

                for (size_t j = 0; j < values->length(); ++j) {
                    const auto* p = reinterpret_cast<const CppType*>(item_data_ptr);
                    if (values->is_null_at(j)) {
                        // bkd do not index null values, so we do nothing here.
                    } else {
                        std::string new_value;
                        size_t value_length = sizeof(CppType);

                        _value_key_coder->full_encode_ascending(p, &new_value);
                        _bkd_writer->add((const uint8_t*)new_value.c_str(), value_length, _rid);
                    }
                    item_data_ptr = (uint8_t*)item_data_ptr + field_size;
                }
                _row_ids_seen_for_bkd++;
                _rid++;
                values++;
            }
        }
        return Status::OK();
    }

    void add_numeric_values(const void* values, size_t count) {
        auto p = reinterpret_cast<const CppType*>(values);
        for (size_t i = 0; i < count; ++i) {
            add_value(*p);
            p++;
            _row_ids_seen_for_bkd++;
        }
    }

    void add_value(const CppType& value) {
        std::string new_value;
        size_t value_length = sizeof(CppType);

        _value_key_coder->full_encode_ascending(&value, &new_value);
        _bkd_writer->add((const uint8_t*)new_value.c_str(), value_length, _rid);

        _rid++;
    }

    int64_t size() const override {
        //TODO: get memory size of inverted index
        return 0;
    }

    int64_t file_size() const override { return _dir->getCompoundFileSize(); }

    void write_null_bitmap(lucene::store::IndexOutput* null_bitmap_out) {
        // write null_bitmap file
        _null_bitmap.runOptimize();
        size_t size = _null_bitmap.getSizeInBytes(false);
        if (size > 0) {
            faststring buf;
            buf.resize(size);
            _null_bitmap.write(reinterpret_cast<char*>(buf.data()), false);
            null_bitmap_out->writeBytes(reinterpret_cast<uint8_t*>(buf.data()), size);
            null_bitmap_out->close();
        }
    }

    Status finish() override {
        std::unique_ptr<lucene::store::IndexOutput> null_bitmap_out = nullptr;
        std::unique_ptr<lucene::store::IndexOutput> data_out = nullptr;
        std::unique_ptr<lucene::store::IndexOutput> index_out = nullptr;
        std::unique_ptr<lucene::store::IndexOutput> meta_out = nullptr;
        try {
            // write bkd file
            if constexpr (field_is_numeric_type(field_type)) {
                _bkd_writer->max_doc_ = _rid;
                _bkd_writer->docs_seen_ = _row_ids_seen_for_bkd;
                null_bitmap_out = std::unique_ptr<lucene::store::IndexOutput>(_dir->createOutput(
                        InvertedIndexDescriptor::get_temporary_null_bitmap_file_name().c_str()));
                data_out = std::unique_ptr<lucene::store::IndexOutput>(_dir->createOutput(
                        InvertedIndexDescriptor::get_temporary_bkd_index_data_file_name().c_str()));
                meta_out = std::unique_ptr<lucene::store::IndexOutput>(_dir->createOutput(
                        InvertedIndexDescriptor::get_temporary_bkd_index_meta_file_name().c_str()));
                index_out = std::unique_ptr<lucene::store::IndexOutput>(_dir->createOutput(
                        InvertedIndexDescriptor::get_temporary_bkd_index_file_name().c_str()));
                write_null_bitmap(null_bitmap_out.get());

                DBUG_EXECUTE_IF("InvertedIndexWriter._set_bkd_data_out_nullptr",
                                { data_out = nullptr; });
                if (data_out != nullptr && meta_out != nullptr && index_out != nullptr) {
                    _bkd_writer->meta_finish(meta_out.get(),
                                             _bkd_writer->finish(data_out.get(), index_out.get()),
                                             int(field_type));
                } else {
                    LOG(WARNING) << "Inverted index writer create output error occurred: nullptr";
                    _CLTHROWA(CL_ERR_IO, "Create output error with nullptr");
                }
                meta_out->close();
                data_out->close();
                index_out->close();
                _dir->close();
            } else if constexpr (field_is_slice_type(field_type)) {
                null_bitmap_out = std::unique_ptr<lucene::store::IndexOutput>(_dir->createOutput(
                        InvertedIndexDescriptor::get_temporary_null_bitmap_file_name().c_str()));
                write_null_bitmap(null_bitmap_out.get());
                close();
                DBUG_EXECUTE_IF(
                        "InvertedIndexWriter._throw_clucene_error_in_fulltext_writer_close", {
                            _CLTHROWA(CL_ERR_IO,
                                      "debug point: test throw error in fulltext index writer");
                        });
            }
        } catch (CLuceneError& e) {
            FINALLY_CLOSE_OUTPUT(null_bitmap_out)
            FINALLY_CLOSE_OUTPUT(meta_out)
            FINALLY_CLOSE_OUTPUT(data_out)
            FINALLY_CLOSE_OUTPUT(index_out)
            if constexpr (field_is_numeric_type(field_type)) {
                FINALLY_CLOSE_OUTPUT(_dir)
            } else if constexpr (field_is_slice_type(field_type)) {
                FINALLY_CLOSE_OUTPUT(_index_writer);
            }
            LOG(WARNING) << "Inverted index writer finish error occurred: " << e.what();
            return Status::Error<doris::ErrorCode::INVERTED_INDEX_CLUCENE_ERROR>(
                    "Inverted index writer finish error occurred");
        }

        return Status::OK();
    }

private:
    rowid_t _rid = 0;
    uint32_t _row_ids_seen_for_bkd = 0;
    roaring::Roaring _null_bitmap;
    uint64_t _reverted_index_size;

    std::unique_ptr<lucene::document::Document> _doc = nullptr;
    lucene::document::Field* _field = nullptr;
    std::unique_ptr<lucene::index::IndexWriter> _index_writer = nullptr;
    std::unique_ptr<lucene::analysis::Analyzer> _analyzer = nullptr;
    std::unique_ptr<lucene::util::Reader> _char_string_reader = nullptr;
    std::shared_ptr<lucene::util::bkd::bkd_writer> _bkd_writer = nullptr;
    std::unique_ptr<DorisCompoundDirectory> _dir = nullptr;
    std::string _segment_file_name;
    std::string _directory;
    io::FileSystemSPtr _fs;
    const KeyCoder* _value_key_coder;
    const TabletIndex* _index_meta;
    InvertedIndexParserType _parser_type;
    std::wstring _field_name;
};

Status InvertedIndexColumnWriter::create(const Field* field,
                                         std::unique_ptr<InvertedIndexColumnWriter>* res,
                                         const std::string& segment_file_name,
                                         const std::string& dir, const TabletIndex* index_meta,
                                         const io::FileSystemSPtr& fs) {
    const auto* typeinfo = field->type_info();
    FieldType type = typeinfo->type();
    std::string field_name = field->name();
    if (type == FieldType::OLAP_FIELD_TYPE_ARRAY) {
        const auto* array_typeinfo = dynamic_cast<const ArrayTypeInfo*>(typeinfo);
        if (array_typeinfo != nullptr) {
            typeinfo = array_typeinfo->item_type_info();
            type = typeinfo->type();
        } else {
            return Status::NotSupported("unsupported array type for inverted index: " +
                                        std::to_string(int(type)));
        }
    }

    switch (type) {
#define M(TYPE)                                                       \
    case TYPE:                                                        \
        *res = std::make_unique<InvertedIndexColumnWriterImpl<TYPE>>( \
                field_name, segment_file_name, dir, fs, index_meta);  \
        break;
        M(FieldType::OLAP_FIELD_TYPE_TINYINT)
        M(FieldType::OLAP_FIELD_TYPE_SMALLINT)
        M(FieldType::OLAP_FIELD_TYPE_INT)
        M(FieldType::OLAP_FIELD_TYPE_UNSIGNED_INT)
        M(FieldType::OLAP_FIELD_TYPE_BIGINT)
        M(FieldType::OLAP_FIELD_TYPE_LARGEINT)
        M(FieldType::OLAP_FIELD_TYPE_CHAR)
        M(FieldType::OLAP_FIELD_TYPE_VARCHAR)
        M(FieldType::OLAP_FIELD_TYPE_STRING)
        M(FieldType::OLAP_FIELD_TYPE_DATE)
        M(FieldType::OLAP_FIELD_TYPE_DATETIME)
        M(FieldType::OLAP_FIELD_TYPE_DECIMAL)
        M(FieldType::OLAP_FIELD_TYPE_DATEV2)
        M(FieldType::OLAP_FIELD_TYPE_DATETIMEV2)
        M(FieldType::OLAP_FIELD_TYPE_DECIMAL32)
        M(FieldType::OLAP_FIELD_TYPE_DECIMAL64)
        M(FieldType::OLAP_FIELD_TYPE_DECIMAL128I)
        M(FieldType::OLAP_FIELD_TYPE_DECIMAL256)
        M(FieldType::OLAP_FIELD_TYPE_BOOL)
        M(FieldType::OLAP_FIELD_TYPE_DOUBLE)
        M(FieldType::OLAP_FIELD_TYPE_FLOAT)
#undef M
    default:
        return Status::NotSupported("unsupported type for inverted index: " +
                                    std::to_string(int(type)));
    }
    if (*res != nullptr) {
        auto st = (*res)->init();
        if (!st.ok()) {
            (*res)->close_on_error();
            return st;
        }
    }
    return Status::OK();
}
} // namespace doris::segment_v2
