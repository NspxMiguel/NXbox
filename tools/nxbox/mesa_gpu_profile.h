#include <algorithm>

/* NXBOX_GPU_PROFILE=1: a GPU timestamp is written before every journaled command of the main
 * list and one more before Close. After the synchronous wait the deltas are attributed to the
 * command that followed each stamp, summed per command kind, and the slowest single commands are
 * published in NXBOX_D3D12_GPUPROF every 40 batches. */
struct NxboxProfile {
  static constexpr unsigned slots = 8192, per_list = 256;
  ID3D12QueryHeap *heap = nullptr;
  ID3D12Resource *readback = nullptr;
  bool failed = false;
  std::mutex lock;
  std::atomic<unsigned> next{0};
  std::unordered_map<std::string, std::pair<unsigned long long, unsigned>> kinds;
  struct Top {
    unsigned long long ticks = 0;
    std::string text;
  } top[6];
  unsigned batches = 0;
  unsigned long long latency_us = 0, run_us = 0, max_latency_us = 0, seen_us = 0, wall_us = 0;
  unsigned long long total = 0;
};
inline NxboxProfile &nxbox_profile() {
  static NxboxProfile profile;
  return profile;
}
inline bool nxbox_profile_on() {
  static const bool on = nxbox_skip_class("NXBOX_GPU_PROFILE");
  return on;
}
inline bool nxbox_profile_ready(ID3D12GraphicsCommandList *commands) {
  auto &p = nxbox_profile();
  if (p.heap && p.readback)
    return true;
  if (p.failed)
    return false;
  std::lock_guard<std::mutex> guard(p.lock);
  if (p.heap && p.readback)
    return true;
  ID3D12Device *dev = nullptr;
  if (FAILED(commands->GetDevice(__uuidof(ID3D12Device), (void **)&dev)) || !dev) {
    p.failed = true;
    return false;
  }
  D3D12_QUERY_HEAP_DESC heap_desc = {D3D12_QUERY_HEAP_TYPE_TIMESTAMP, NxboxProfile::slots, 0};
  D3D12_HEAP_PROPERTIES props = {D3D12_HEAP_TYPE_READBACK, D3D12_CPU_PAGE_PROPERTY_UNKNOWN,
                                 D3D12_MEMORY_POOL_UNKNOWN, 1, 1};
  D3D12_RESOURCE_DESC desc = {};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  desc.Width = NxboxProfile::slots * 8;
  desc.Height = desc.DepthOrArraySize = desc.MipLevels = 1;
  desc.SampleDesc.Count = 1;
  desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  ID3D12QueryHeap *heap = nullptr;
  ID3D12Resource *readback = nullptr;
  if (SUCCEEDED(dev->CreateQueryHeap(&heap_desc, __uuidof(ID3D12QueryHeap), (void **)&heap)) &&
      SUCCEEDED(dev->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &desc,
                                             D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                             __uuidof(ID3D12Resource), (void **)&readback))) {
    p.heap = heap;
    p.readback = readback;
  } else {
    p.failed = true;
    if (heap)
      heap->Release();
  }
  dev->Release();
  return !p.failed;
}
#undef NXBOX_PROFILE_STAMP
#define NXBOX_PROFILE_CLOSE(commands, journal) nxbox_profile_close(commands, journal)
#define NXBOX_PROFILE_COLLECT(queue, journal) nxbox_profile_collect(queue, journal)
#define NXBOX_PROFILE_SUBMIT(journal) nxbox_profile_submit(journal)
#define NXBOX_PROFILE_STAMP(journal, commands) nxbox_profile_stamp(journal, commands)
inline void nxbox_profile_stamp(NxboxBatchJournal *journal,
                                ID3D12GraphicsCommandList *commands) {
  if (!journal || !nxbox_profile_on() || journal->prof_count + 2 >= NxboxProfile::per_list ||
      !nxbox_profile_ready(commands))
    return;
  auto &p = nxbox_profile();
  if (journal->prof_count == 0)
    journal->prof_base = p.next.fetch_add(NxboxProfile::per_list) % NxboxProfile::slots;
  commands->EndQuery(p.heap, D3D12_QUERY_TYPE_TIMESTAMP, journal->prof_base + journal->prof_count);
  journal->stamp_entry.push_back(journal->entries.size());
  ++journal->prof_count;
}
inline void nxbox_profile_close(ID3D12GraphicsCommandList *commands, NxboxBatchJournal *journal) {
  if (!journal || !journal->prof_count || !nxbox_profile_on())
    return;
  auto &p = nxbox_profile();
  commands->EndQuery(p.heap, D3D12_QUERY_TYPE_TIMESTAMP, journal->prof_base + journal->prof_count);
  commands->ResolveQueryData(p.heap, D3D12_QUERY_TYPE_TIMESTAMP, journal->prof_base,
                             journal->prof_count + 1, p.readback, (UINT64)journal->prof_base * 8);
}
inline void nxbox_profile_submit(NxboxBatchJournal *journal) {
  if (!journal || !nxbox_profile_on())
    return;
  LARGE_INTEGER now;
  QueryPerformanceCounter(&now);
  journal->prof_submit_qpc = now.QuadPart;
}
inline void nxbox_profile_collect(ID3D12CommandQueue *queue, const NxboxBatchJournal *journal) {
  if (!journal || !journal->prof_count || !nxbox_profile_on())
    return;
  auto &p = nxbox_profile();
  if (!p.readback)
    return;
  D3D12_RANGE read = {(SIZE_T)journal->prof_base * 8,
                      (SIZE_T)(journal->prof_base + journal->prof_count + 1) * 8};
  void *data = nullptr;
  if (FAILED(p.readback->Map(0, &read, &data)) || !data)
    return;
  const uint64_t *t = (const uint64_t *)data + journal->prof_base;
  std::lock_guard<std::mutex> guard(p.lock);
  for (unsigned i = 0; i < journal->prof_count; ++i) {
    const unsigned long long dt = t[i + 1] > t[i] ? t[i + 1] - t[i] : 0;
    std::string text = journal->stamp_entry[i] < journal->entries.size()
                           ? journal->entries[journal->stamp_entry[i]]
                           : std::string("unjournaled");
    // "<seq> main <Command> ..." -> kind is the third word.
    size_t a = text.find(' '), b = a == std::string::npos ? a : text.find(' ', a + 1);
    size_t c = b == std::string::npos ? b : text.find(' ', b + 1);
    std::string kind = b == std::string::npos ? text : text.substr(b + 1, c == std::string::npos ? c : c - b - 1);
    auto &k = p.kinds[kind];
    k.first += dt;
    ++k.second;
    for (auto &top : p.top) {
      if (dt > top.ticks) {
        if (&top != &p.top[5])
          for (int j = 5; j > (int)(&top - p.top); --j)
            p.top[j] = p.top[j - 1];
        top.ticks = dt;
        top.text = text.substr(0, 170);
        break;
      }
    }
  }
  p.total += t[journal->prof_count] > t[0] ? t[journal->prof_count] - t[0] : 0;
  {
    // When the list started on the GPU, relative to the CPU's Execute: a long latency means the
    // queue was blocked before the list ran (waits, preemption), not that the list was slow.
    UINT64 gpu_now = 0, cpu_now = 0, freq = 0;
    LARGE_INTEGER qpc_freq;
    if (SUCCEEDED(queue->GetTimestampFrequency(&freq)) && freq &&
        SUCCEEDED(queue->GetClockCalibration(&gpu_now, &cpu_now)) &&
        QueryPerformanceFrequency(&qpc_freq) && qpc_freq.QuadPart) {
      const double since_start = double(gpu_now - t[0]) / double(freq);
      const double start_cpu = double(cpu_now) / double(qpc_freq.QuadPart) - since_start;
      const double submit = double(journal->prof_submit_qpc) / double(qpc_freq.QuadPart);
      const double latency = start_cpu - submit;
      if (latency > 0 && latency < 60) {
        const unsigned long long us = (unsigned long long)(latency * 1e6);
        p.latency_us += us;
        if (us > p.max_latency_us)
          p.max_latency_us = us;
        p.run_us += (unsigned long long)(double(t[journal->prof_count] - t[0]) * 1e6 / double(freq));
        // From the list's last GPU timestamp to now (the CPU has just seen the fence complete),
        // and the whole Execute-to-now wall time.
        p.seen_us += (unsigned long long)(double(gpu_now - t[journal->prof_count]) * 1e6 / double(freq));
        p.wall_us += (unsigned long long)((double(cpu_now) / double(qpc_freq.QuadPart) - submit) * 1e6);
      }
    }
  }
  D3D12_RANGE none = {0, 0};
  p.readback->Unmap(0, &none);
  if (++p.batches % 40)
    return;
  UINT64 freq = 0;
  queue->GetTimestampFrequency(&freq);
  std::vector<std::pair<unsigned long long, std::string>> sorted;
  for (auto &k : p.kinds)
    sorted.push_back({k.second.first, k.first + "=" + std::to_string(k.second.first) + "/" +
                                          std::to_string(k.second.second)});
  std::sort(sorted.begin(), sorted.end(), [](auto &x, auto &y) { return x.first > y.first; });
  std::string out = "freq=" + std::to_string(freq) + " batches=40 total=" + std::to_string(p.total) +
                    " start_latency_us=" + std::to_string(p.latency_us) + " max_latency_us=" +
                    std::to_string(p.max_latency_us) + " run_us=" + std::to_string(p.run_us) +
                    " end_to_seen_us=" + std::to_string(p.seen_us) + " wall_us=" + std::to_string(p.wall_us);
  for (size_t i = 0; i < sorted.size() && i < 10; ++i)
    out += " " + sorted[i].second;
  for (auto &top : p.top)
    if (top.ticks)
      out += " | " + std::to_string(top.ticks) + " " + top.text;
  if (out.size() > 3800)
    out.resize(3800);
  SetEnvironmentVariableA("NXBOX_D3D12_GPUPROF", out.c_str());
  p.kinds.clear();
  p.total = 0;
  p.latency_us = p.run_us = p.max_latency_us = p.seen_us = p.wall_us = 0;
  for (auto &top : p.top)
    top = {};
}

