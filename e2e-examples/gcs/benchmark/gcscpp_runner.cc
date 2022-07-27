#include "gcscpp_runner.h"

#include <string>
#include <thread>

#include "absl/random/random.h"
#include "absl/time/time.h"
#include "google/cloud/storage/client.h"
#include "google/cloud/storage/grpc_plugin.h"

GcscppRunner::GcscppRunner(Parameters parameters,
                           std::shared_ptr<RunnerWatcher> watcher)
    : parameters_(parameters),
      object_resolver_(parameters_.object, parameters_.object_format,
                       parameters_.object_start, parameters_.object_stop),
      watcher_(watcher) {}

bool GcscppRunner::Run() {
  auto client = ::google::cloud::storage::Client();
  if (parameters_.client == "gcscpp-grpc") {
    client = ::google::cloud::storage_experimental::DefaultGrpcClient();
  } else {
    client = ::google::cloud::storage::Client();
  }

  // Spawns benchmark threads and waits until they're done.
  std::vector<std::thread> threads;
  std::vector<bool> returns(parameters_.threads);
  for (int i = 1; i <= parameters_.threads; i++) {
    int thread_id = i;
    threads.emplace_back([thread_id, client, &returns, this]() {
      returns[thread_id - 1] = this->DoOperation(thread_id, client);
    });
  }
  std::for_each(threads.begin(), threads.end(),
                [](std::thread& t) { t.join(); });
  return std::all_of(returns.begin(), returns.end(), [](bool v) { return v; });
}

bool GcscppRunner::DoOperation(int thread_id,
                               google::cloud::storage::Client storage_client) {
  switch (parameters_.operation_type) {
    case OperationType::Read:
      return DoRead(thread_id, storage_client);
    case OperationType::RandomRead:
      return DoRandomRead(thread_id, storage_client);
    case OperationType::Write:
      return DoWrite(thread_id, storage_client);
    default:
      return false;
  }
}

bool GcscppRunner::DoRead(int thread_id,
                          google::cloud::storage::Client storage_client) {
  for (int run = 0; run < parameters_.runs; run++) {
    std::string object = object_resolver_.Resolve(thread_id, run);

    absl::Time run_start = absl::Now();
    auto reader = storage_client.ReadObject(parameters_.bucket, object);
    if (!reader) {
      std::cerr << "Error reading object: " << reader.status() << "\n";
      return false;
    }

    std::string contents{std::istreambuf_iterator<char>{reader}, {}};
    int64_t total_bytes = contents.size();
    absl::Time run_end = absl::Now();

    watcher_->NotifyCompleted(OperationType::Read, thread_id, 0, "",
                              parameters_.bucket, object, grpc::Status::OK,
                              total_bytes, run_start, run_end - run_start, {});
  }

  return true;
}

bool GcscppRunner::DoRandomRead(int thread_id,
                                google::cloud::storage::Client storage_client) {
  if (parameters_.read_limit <= 0) {
    std::cerr << "read_limit should be greater than 0." << std::endl;
    return false;
  }
  int64_t read_span =
      parameters_.read_limit - std::max(int64_t(0), parameters_.read_offset);
  if (read_span <= 0) {
    std::cerr << "read_limit should be greater than read_offset." << std::endl;
    return false;
  }
  if (parameters_.chunk_size == 0) {
    std::cerr << "chunk_size should be greater than 0." << std::endl;
    return false;
  }
  int64_t chunks = read_span / parameters_.chunk_size;
  if (chunks <= 0) {
    std::cerr
        << "read_limit should be greater than or equal to readable window."
        << std::endl;
    return false;
  }

  std::string object = object_resolver_.Resolve(thread_id, 0);
  absl::BitGen gen;
  std::vector<char> buf(parameters_.chunk_size);
  for (int run = 0; run < parameters_.runs; run++) {
    int64_t offset = absl::Uniform(gen, 0, chunks) * parameters_.chunk_size;
    absl::Time run_start = absl::Now();
    auto reader = storage_client.ReadObject(
        parameters_.bucket, object,
        google::cloud::storage::ReadFromOffset(offset));
    if (!reader) {
      std::cerr << "Error reading object: " << reader.status() << "\n";
      return false;
    }
    int64_t total_bytes = 0;
    reader.read(buf.data(), buf.size());
    total_bytes += buf.size();
    reader.Close();
    absl::Time run_end = absl::Now();

    watcher_->NotifyCompleted(OperationType::Read, thread_id, 0, "",
                              parameters_.bucket, object, grpc::Status::OK,
                              total_bytes, run_start, run_end - run_start, {});
  }

  return true;
}

static std::vector<char> GetRandomData(size_t size) {
  std::vector<char> content(size);
  int* const s = reinterpret_cast<int*>(&(*content.begin()));
  int* const e = reinterpret_cast<int*>(&(*content.rbegin()));
  for (int* c = s; c < e; c += 1) {
    *c = rand();
  }
  return content;
}

bool GcscppRunner::DoWrite(int thread_id,
                           google::cloud::storage::Client storage_client) {
  const int64_t max_chunk_size =
      (parameters_.chunk_size < 0) ? 2097152 : parameters_.chunk_size;
  const std::vector<char> content = GetRandomData(max_chunk_size);

  if (parameters_.object_stop > 0) {
    std::cerr << "write doesn't support object_stop" << std::endl;
    return false;
  }
  if (parameters_.write_size <= 0) {
    std::cerr << "write_size should be greater than 0." << std::endl;
    return false;
  }

  for (int run = 0; run < parameters_.runs; run++) {
    std::string object = object_resolver_.Resolve(thread_id, run);
    absl::Time run_start = absl::Now();

    int64_t total_bytes = 0;
    std::vector<RunnerWatcher::Chunk> chunks;
    chunks.reserve(256);

    auto writer = storage_client.WriteObject(parameters_.bucket, object);
    for (int64_t o = 0; o < parameters_.write_size; o += max_chunk_size) {
      int64_t chunk_size = std::min(max_chunk_size, parameters_.write_size - o);
      writer.write(content.data(), static_cast<std::streamsize>(chunk_size));

      RunnerWatcher::Chunk chunk = {absl::Now(), chunk_size};
      chunks.push_back(chunk);
      total_bytes += chunk_size;
    }
    writer.Close();

    absl::Time run_end = absl::Now();

    watcher_->NotifyCompleted(OperationType::Write, thread_id, 0, "",
                              parameters_.bucket, object, grpc::Status::OK,
                              total_bytes, run_start, run_end - run_start,
                              std::move(chunks));
  }

  return true;
}