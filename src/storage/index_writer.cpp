#include "dse/storage/index_writer.hpp"
#include <algorithm>
#include <set>
#include <system_error>
#include <utility>
namespace dse::storage {
namespace {
WriterError failure(WriterErrorCode code,std::string message){return {code,std::move(message)};}
bool same_schema(const index::IndexSchema& left,const index::IndexSchema& right){
 if(left.fields().size()!=right.fields().size())return false;
 auto a=left.fields().begin();auto b=right.fields().begin();
 for(;a!=left.fields().end();++a,++b){const auto& x=a->second;const auto& y=b->second;
  if(x.name!=y.name||x.type!=y.type||x.indexed!=y.indexed||x.stored!=y.stored||x.boost!=y.boost)return false;
  if(static_cast<bool>(x.analyzer)!=static_cast<bool>(y.analyzer))return false;
  if(x.analyzer&&x.analyzer->descriptor()!=y.analyzer->descriptor())return false;
 }
 return true;
}
std::expected<void,WriterError> cleanup_startup_files(const std::filesystem::path& directory,const IndexManifest& current){
 std::set<std::string,std::less<>> retained;for(const auto& segment:current.segments)retained.insert(segment.filename);
 const std::string current_manifest="MANIFEST-"+std::to_string(current.generation.value());std::error_code ec;
 for(std::filesystem::directory_iterator it(directory,ec),end;!ec&&it!=end;it.increment(ec)){
  if(!it->is_regular_file()) continue;
  const auto name=it->path().filename().string();
  bool remove=false;
  if(name.ends_with(".tmp"))remove=true;
  else if(name.starts_with("MANIFEST-")&&name!=current_manifest)remove=true;
  else if(name.starts_with("segment-")&&name.ends_with(".dseg")&&!retained.contains(name))remove=true;
  if(remove){std::error_code removal;std::filesystem::remove(it->path(),removal);if(removal)return std::unexpected(failure(WriterErrorCode::storage_error,"cannot reclaim "+name+": "+removal.message()));}
 }
 if(ec)return std::unexpected(failure(WriterErrorCode::storage_error,"cannot scan index directory: "+ec.message()));
 return {};
}
}
IndexWriter::IndexWriter(std::filesystem::path directory,index::IndexSchema schema,IndexWriterOptions options):directory_(std::move(directory)),schema_(std::move(schema)),options_(options),store_(directory_),wal_(directory_/"WRITE-AHEAD.log"),active_(std::make_unique<index::InMemoryIndex>(schema_)){}
IndexWriter::~IndexWriter(){(void)refresh();{std::lock_guard lock(mutex_);stopping_=true;}condition_.notify_all();if(worker_.joinable())worker_.join();}
std::expected<std::unique_ptr<IndexWriter>,WriterError> IndexWriter::open(std::filesystem::path directory,index::IndexSchema schema,IndexWriterOptions options){
 if(options.maximum_buffered_mutations==0U||options.maximum_frozen_indexes==0U||options.merge_width<2U||(options.automatic_merge_segment_count!=0U&&options.automatic_merge_segment_count<2U))return std::unexpected(failure(WriterErrorCode::storage_error,"invalid writer limits"));
 std::error_code ec;std::filesystem::create_directories(directory,ec);if(ec)return std::unexpected(failure(WriterErrorCode::storage_error,ec.message()));
 auto writer=std::unique_ptr<IndexWriter>(new IndexWriter(std::move(directory),std::move(schema),options));
 if(std::filesystem::exists(writer->directory_/"CURRENT")){auto opened=writer->store_.open_current();if(!opened)return std::unexpected(failure(WriterErrorCode::storage_error,opened.error().message));writer->manifest_=opened->manifest();for(const auto& segment:opened->segments()){if(!same_schema(segment->schema(),writer->schema_))return std::unexpected(failure(WriterErrorCode::storage_error,"writer schema differs from persisted schema"));writer->next_segment_id_=std::max(writer->next_segment_id_,segment->segment_id().value()+1U);for(const auto& [id,record]:segment->records())writer->versions_[id]=std::max(writer->versions_[id],record.document.version);}}
 if(options.reclaim_obsolete_files){auto cleanup=cleanup_startup_files(writer->directory_,writer->manifest_);if(!cleanup)return std::unexpected(cleanup.error());}
 auto replay=writer->wal_.replay();if(!replay)return std::unexpected(failure(WriterErrorCode::storage_error,replay.error().message));
 for(auto& document:*replay){if(const auto found=writer->versions_.find(document.id);found!=writer->versions_.end()&&document.version<=found->second)continue;const auto id=document.id;const auto version=document.version;auto restored=writer->active_->put(std::move(document));if(!restored)return std::unexpected(failure(WriterErrorCode::storage_error,"WAL replay failed: "+restored.error().message));writer->versions_[id]=version;++writer->buffered_mutations_;}
 writer->worker_=std::thread(&IndexWriter::worker_loop,writer.get());return writer;
}
std::expected<void,WriterError> IndexWriter::wait_for_capacity_locked(std::unique_lock<std::mutex>& lock){condition_.wait(lock,[&]{return worker_error_.has_value()||stopping_||frozen_.size()<options_.maximum_frozen_indexes;});if(worker_error_)return std::unexpected(*worker_error_);if(stopping_)return std::unexpected(failure(WriterErrorCode::closed,"writer is closed"));return {};}
void IndexWriter::freeze_active_locked(){if(buffered_mutations_==0U)return;frozen_.push_back(active_->snapshot());active_=std::make_unique<index::InMemoryIndex>(schema_);buffered_mutations_=0U;condition_.notify_all();}
std::expected<void,WriterError> IndexWriter::put(Document document){std::unique_lock lock(mutex_);condition_.wait(lock,[&]{return !refreshing_||worker_error_.has_value()||stopping_;});if(worker_error_)return std::unexpected(*worker_error_);if(stopping_)return std::unexpected(failure(WriterErrorCode::closed,"writer is closed"));if(buffered_mutations_>=options_.maximum_buffered_mutations){if(auto capacity=wait_for_capacity_locked(lock);!capacity)return capacity;freeze_active_locked();}if(const auto it=versions_.find(document.id);it!=versions_.end()&&document.version<=it->second)return std::unexpected(failure(WriterErrorCode::stale_version,"document version is not newer"));const auto persisted=document;const auto id=document.id;const auto version=document.version;auto result=active_->put(std::move(document));if(!result)return std::unexpected(failure(WriterErrorCode::index_error,result.error().message));auto logged=wal_.append(persisted);if(!logged){worker_error_=failure(WriterErrorCode::storage_error,logged.error().message);condition_.notify_all();return std::unexpected(*worker_error_);}versions_[id]=version;++buffered_mutations_;if(buffered_mutations_>=options_.maximum_buffered_mutations){if(auto capacity=wait_for_capacity_locked(lock);!capacity)return capacity;freeze_active_locked();}return {};}
std::expected<void,WriterError> IndexWriter::erase(const DocumentId& id,std::uint64_t version){return put({.id=id,.fields={},.stored_metadata={},.version=version,.deleted=true});}
std::expected<void,WriterError> IndexWriter::publish_snapshot(index::IndexSnapshot snapshot){std::size_t segment_count{};{std::lock_guard publication(publication_mutex_);SegmentId id(1);IndexManifest next;{std::lock_guard lock(mutex_);id=SegmentId(next_segment_id_++);next={GenerationId(manifest_.generation.value()+1U),manifest_.segments};}const std::string filename="segment-"+std::to_string(id.value())+".dseg";auto write=SegmentWriter::write(directory_/filename,snapshot,{.segment_id=id});if(!write)return std::unexpected(failure(WriterErrorCode::storage_error,write.error().message));next.segments.push_back({id,filename});auto published=store_.publish(next);if(!published)return std::unexpected(failure(WriterErrorCode::storage_error,published.error().message));segment_count=next.segments.size();{std::lock_guard lock(mutex_);manifest_=std::move(next);}}if(options_.automatic_merge_segment_count!=0U&&segment_count>=options_.automatic_merge_segment_count)return compact_published();return {};}
void IndexWriter::worker_loop(){for(;;){std::optional<index::IndexSnapshot> snapshot;{std::unique_lock lock(mutex_);condition_.wait(lock,[&]{return stopping_||!frozen_.empty();});if(frozen_.empty()&&stopping_)return;snapshot.emplace(std::move(frozen_.front()));frozen_.pop_front();worker_busy_=true;condition_.notify_all();}auto result=publish_snapshot(std::move(*snapshot));{std::lock_guard lock(mutex_);worker_busy_=false;if(!result)worker_error_=result.error();}condition_.notify_all();if(!result)return;}}
std::expected<void,WriterError> IndexWriter::wait_until_idle_locked(std::unique_lock<std::mutex>& lock){condition_.wait(lock,[&]{return worker_error_.has_value()||(!worker_busy_&&frozen_.empty());});if(worker_error_)return std::unexpected(*worker_error_);return {};}
std::expected<void,WriterError> IndexWriter::refresh(){std::unique_lock lock(mutex_);condition_.wait(lock,[&]{return !refreshing_||worker_error_.has_value();});if(worker_error_)return std::unexpected(*worker_error_);refreshing_=true;const auto finish=[&]{refreshing_=false;condition_.notify_all();};if(buffered_mutations_!=0U){if(auto capacity=wait_for_capacity_locked(lock);!capacity){finish();return capacity;}freeze_active_locked();}auto idle=wait_until_idle_locked(lock);if(!idle){finish();return idle;}auto reset=wal_.reset();if(!reset){worker_error_=failure(WriterErrorCode::storage_error,reset.error().message);finish();return std::unexpected(*worker_error_);}finish();return {};}
std::expected<GenerationView,WriterError> IndexWriter::open_search_view()const{std::lock_guard lock(mutex_);if(worker_error_)return std::unexpected(*worker_error_);auto opened=store_.open_current();if(!opened)return std::unexpected(failure(WriterErrorCode::storage_error,opened.error().message));auto view=GenerationView::open(std::move(*opened));if(!view)return std::unexpected(failure(WriterErrorCode::storage_error,view.error().message));return std::move(*view);}
void IndexWriter::reclaim(const IndexManifest& obsolete,const IndexManifest& replacement)const{if(!options_.reclaim_obsolete_files)return;std::set<std::string,std::less<>> retained;for(const auto& item:replacement.segments)retained.insert(item.filename);std::error_code ignored;for(const auto& item:obsolete.segments)if(!retained.contains(item.filename))std::filesystem::remove(directory_/item.filename,ignored);if(obsolete.generation.value()!=0U)std::filesystem::remove(directory_/("MANIFEST-"+std::to_string(obsolete.generation.value())),ignored);}
std::expected<void,WriterError> IndexWriter::compact_published(){
 std::lock_guard publication(publication_mutex_);IndexManifest obsolete;{std::lock_guard lock(mutex_);if(manifest_.segments.size()<=1U)return {};obsolete=manifest_;}
 auto opened=store_.open_current();if(!opened)return std::unexpected(failure(WriterErrorCode::storage_error,opened.error().message));
 struct Candidate{std::size_t index;std::uintmax_t bytes;};std::vector<Candidate> candidates;candidates.reserve(obsolete.segments.size());
 for(std::size_t i=0;i<obsolete.segments.size();++i){std::error_code ec;const auto bytes=std::filesystem::file_size(directory_/obsolete.segments[i].filename,ec);if(ec)return std::unexpected(failure(WriterErrorCode::storage_error,ec.message()));candidates.push_back({i,bytes});}
 std::sort(candidates.begin(),candidates.end(),[](const Candidate& a,const Candidate& b){return a.bytes==b.bytes?a.index<b.index:a.bytes<b.bytes;});
 const auto merge_count=std::min(options_.merge_width,candidates.size());std::set<std::size_t> selected_indices;std::vector<std::shared_ptr<const SegmentReader>> selected;
 for(std::size_t i=0;i<merge_count;++i){selected_indices.insert(candidates[i].index);selected.push_back(opened->segments()[candidates[i].index]);}
 SegmentId id(1);{std::lock_guard lock(mutex_);id=SegmentId(next_segment_id_++);}auto merged=SegmentMerger::merge_segments(selected,directory_,id);if(!merged)return std::unexpected(failure(WriterErrorCode::storage_error,merged.error().message));
 IndexManifest next{GenerationId(obsolete.generation.value()+1U),{}};for(std::size_t i=0;i<obsolete.segments.size();++i)if(!selected_indices.contains(i))next.segments.push_back(obsolete.segments[i]);next.segments.push_back(*merged);
 std::sort(next.segments.begin(),next.segments.end(),[](const ManifestSegment& a,const ManifestSegment& b){return a.id<b.id;});auto publish=store_.publish(next);if(!publish)return std::unexpected(failure(WriterErrorCode::storage_error,publish.error().message));{std::lock_guard lock(mutex_);manifest_=next;}reclaim(obsolete,next);return {};
}
std::expected<void,WriterError> IndexWriter::merge_all(){if(auto visible=refresh();!visible)return visible;for(;;){{std::lock_guard lock(mutex_);if(manifest_.segments.size()<=1U)return {};}if(auto merged=compact_published();!merged)return merged;}}
GenerationId IndexWriter::generation()const noexcept{std::lock_guard lock(mutex_);return manifest_.generation;}
IndexWriterStatistics IndexWriter::statistics()const noexcept{std::lock_guard lock(mutex_);return {.buffered_mutations=buffered_mutations_,.frozen_indexes=frozen_.size(),.flush_in_progress=worker_busy_,.published_segments=manifest_.segments.size(),.generation=manifest_.generation};}
}  // namespace dse::storage
