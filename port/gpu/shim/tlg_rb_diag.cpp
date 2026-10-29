// SPDX-License-Identifier: GPL-2.0-or-later
// tlg diag, not for commit: see tlg_rb_diag.h.
#include "tlg_rb_diag.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#undef near
#else
#include <ucontext.h>
#endif

#include "tlg_freeze.h"

extern "C" int runtime_memory_read_backing(uintptr_t address, void* data, uint64_t size);

namespace TlgRbDiag {
namespace {
constexpr u64 WindowNs = 5'000'000'000ull;
constexpr u64 GranuleShift = 16; // write records per 64 KiB

thread_local Ctx* current = nullptr;
thread_local u64 current_producer = 0; // kind << 56 | hash

struct Site {
    u64 rip = 0, caller = 0, frames[4]{};
    bool write = false, internal = false;
    u64 n = 0, ns = 0, max_ns = 0, queue = 0, record = 0, wait = 0, writeback = 0, image = 0,
        diag = 0;
    u64 outcome[NumOutcomes]{};
    u64 downloads = 0, copies = 0, bytes = 0, changed = 0, hot_bytes = 0, hot_changed = 0;
    u64 copied = 0;   // faults whose download copied bytes
    u64 unchanged = 0; // ... of which no byte differed from what the CPU saw
    u64 hit = 0, page = 0; // faults whose faulting 8 bytes / page changed
    u64 state[4]{};
    u64 min_age = ~0ull, age_sum = 0, age_n = 0;
    u64 label[NumLabelKinds + 1]{};
    u64 label_age_sum = 0;
    u64 last_addr = 0;
    std::unordered_map<u32, u64> producers; // producer id -> changed bytes
};

struct Image {
    u32 width = 0, height = 0, bits = 0, format = 0;
    bool render_target = false;
    u64 size = 0;
    u64 queued = 0, bytes = 0, landed = 0, changed = 0, changed_copies = 0, skipped = 0;
    u64 cpu_read = 0, cpu_write = 0, gpu = 0, wait_ns = 0;
    std::unordered_map<u64, u64> sites; // CPU site -> accesses while pending
};

struct Resolve {
    u64 n = 0, pending = 0, waited = 0, ns = 0, bytes = 0;
};

struct WriteRec {
    u64 start = 0, end = 0, t = 0;
    u32 producer = 0;
};
struct Granule {
    WriteRec rec[4];
    u32 next = 0;
};

std::mutex mutex; // sites, images, resolves, finishes, trace
std::unordered_map<u64, Site> sites;
std::unordered_map<u64, Image> images;
Resolve resolves[16];
u64 finish_n[3]{}, finish_ns[3]{};
u64 window_start = 0;
std::atomic<u64> next_print{0};
std::atomic<u64> labels[NumLabelKinds]{};
std::atomic<u64> label_t[NumLabelKinds]{};
std::FILE* trace = nullptr;

std::mutex write_mutex; // producers, writes
std::vector<u64> producers{0}; // id -> kind << 56 | key
std::unordered_map<u64, u32> producer_ids;
std::unordered_map<u64, Granule> writes;

struct Control {
    std::unordered_set<u64> skip, only, skipimg, onlyimg;
    bool skipr = false, skipw = false;
};
std::mutex control_mutex;
Control control;
std::string control_text;
std::atomic<u64> control_next{0};

const char* ProducerName(u32 kind) {
    static const char* names[] = {"none", "ps", "hs", "ds", "vs", "gs", "cs",
                                  "fill", "dma", "hle", "other"};
    return kind < NumProducerKinds ? names[kind] : "?";
}

const char* LabelName(u32 kind) {
    static const char* names[] = {"eop", "eos", "wd", "rm", "cwd", "none"};
    return kind <= NumLabelKinds ? names[kind] : "?";
}

const char* SourceName(u32 source) {
    static const char* names[] = {"unknown", "buffer_obtain", "buffer_for_image", "buffer_upload",
                                  "cpu_write", "cpu_read", "image_download", "gpu_partial_write",
                                  "image_refresh", "cache_cleanup", "image_free"};
    return source < 11 ? names[source] : "?";
}

// guest offsets as g:<hex>, host code as module+offset, else the raw address.
std::string Describe(u64 at) {
    char text[160];
    const u64 base = TlgFreeze::guest_base.load(std::memory_order_relaxed);
    if (base && at - base < TlgFreeze::guest_size.load(std::memory_order_relaxed)) {
        std::snprintf(text, sizeof(text), "g:%llx", (unsigned long long)(at - base));
        return text;
    }
#ifdef _WIN32
    HMODULE module = nullptr;
    char name[MAX_PATH];
    if (at && GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                     GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                 reinterpret_cast<LPCSTR>(at), &module) &&
        module && GetModuleFileNameA(module, name, sizeof(name))) {
        const char* file = std::strrchr(name, '\\');
        std::snprintf(text, sizeof(text), "%s+%llx", file ? file + 1 : name,
                      (unsigned long long)(at - reinterpret_cast<u64>(module)));
        return text;
    }
#endif
    std::snprintf(text, sizeof(text), "%llx", (unsigned long long)at);
    return text;
}

u64 GuestOffset(u64 at) {
    const u64 base = TlgFreeze::guest_base.load(std::memory_order_relaxed);
    return base && at - base < TlgFreeze::guest_size.load(std::memory_order_relaxed) ? at - base
                                                                                      : at;
}

std::string ProducerText(u32 id) {
    u64 p;
    {
        std::scoped_lock lock{write_mutex};
        p = id < producers.size() ? producers[id] : 0;
    }
    const u32 kind = u32(p >> 56);
    const u64 key = p & ((1ull << 56) - 1);
    char text[200];
    if (kind == ProdOther) {
        std::snprintf(text, sizeof(text), "other@%s", Describe(key).c_str());
    } else if (kind >= ProdFragment && kind <= ProdCompute) {
        std::snprintf(text, sizeof(text), "%s:%llx", ProducerName(kind), (unsigned long long)key);
    } else {
        std::snprintf(text, sizeof(text), "%s", ProducerName(kind));
    }
    return text;
}

u32 ProducerId(u64 p) { // under write_mutex
    const auto [it, added] = producer_ids.try_emplace(p, u32(producers.size()));
    if (added) {
        producers.push_back(p);
    }
    return it->second;
}

void ReloadControl() {
    static const char* path = std::getenv("TLG_RB_CONTROL");
    if (!path) {
        return;
    }
    const u64 now = NowNs();
    u64 due = control_next.load(std::memory_order_relaxed);
    if (now < due || !control_next.compare_exchange_strong(due, now + 500'000'000ull)) {
        return;
    }
    std::string text;
    if (std::FILE* f = std::fopen(path, "rb")) {
        char buf[4096];
        size_t n;
        while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) {
            text.append(buf, n);
        }
        std::fclose(f);
    }
    std::scoped_lock lock{control_mutex};
    if (text == control_text) {
        return;
    }
    control_text = text;
    Control c;
    size_t at = 0;
    while (at < text.size()) {
        size_t end = text.find('\n', at);
        if (end == std::string::npos) {
            end = text.size();
        }
        std::string line = text.substr(at, end - at);
        at = end + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) {
            line.pop_back();
        }
        if (line.rfind("mark ", 0) == 0) {
            std::printf("RBD mark t=%.3f %s\n", now / 1e9, line.c_str() + 5);
            continue;
        }
        char word[16] = {};
        int used = 0;
        if (std::sscanf(line.c_str(), "%15s%n", word, &used) != 1) {
            continue;
        }
        std::unordered_set<u64>* set = nullptr;
        if (!std::strcmp(word, "skip")) set = &c.skip;
        else if (!std::strcmp(word, "only")) set = &c.only;
        else if (!std::strcmp(word, "skipimg")) set = &c.skipimg;
        else if (!std::strcmp(word, "onlyimg")) set = &c.onlyimg;
        else if (!std::strcmp(word, "skipr")) c.skipr = true;
        else if (!std::strcmp(word, "skipw")) c.skipw = true;
        if (set) {
            const char* p = line.c_str() + used;
            char* next;
            for (u64 v = std::strtoull(p, &next, 16); next != p; v = std::strtoull(p, &next, 16)) {
                set->insert(v);
                p = next;
            }
        }
    }
    control = std::move(c);
    std::printf("RBD control t=%.3f skip=%zu only=%zu skipimg=%zu onlyimg=%zu skipr=%d skipw=%d\n",
                now / 1e9, control.skip.size(), control.only.size(), control.skipimg.size(),
                control.onlyimg.size(), control.skipr, control.skipw);
    std::fflush(stdout);
}

void PrintWindow(u64 now) { // under mutex
    const double seconds = (now - window_start) / 1e9;
    u64 n = 0, ns = 0;
    for (const auto& [key, s] : sites) {
        n += s.n;
        ns += s.ns;
    }
    std::printf("RBD window t=%.3f s=%.2f faults=%llu ms=%.1f sites=%zu", now / 1e9, seconds,
                (unsigned long long)n, ns / 1e6, sites.size());
    for (u32 k = 0; k < NumLabelKinds; ++k) {
        std::printf(" %s=%llu", LabelName(k),
                    (unsigned long long)labels[k].exchange(0, std::memory_order_relaxed));
    }
    std::printf("\n");
    std::vector<const Site*> order;
    for (const auto& [key, s] : sites) {
        order.push_back(&s);
    }
    std::ranges::sort(order, [](const Site* a, const Site* b) { return a->ns > b->ns; });
    for (const Site* s : order) {
        std::string site = s->internal ? "host" : Describe(s->rip);
        std::string caller = s->internal ? Describe(s->frames[0]) : Describe(s->caller);
        if (s->internal) {
            for (int i = 1; i < 4 && s->frames[i]; ++i) {
                caller += "<" + Describe(s->frames[i]);
            }
        }
        std::printf("RBD site %c %s caller=%s n=%llu ms=%.2f max_us=%.0f queue_ms=%.2f "
                    "rec_ms=%.2f wait_ms=%.2f wb_ms=%.2f img_ms=%.2f diag_ms=%.2f",
                    s->write ? 'W' : 'R', site.c_str(), caller.c_str(), (unsigned long long)s->n,
                    s->ns / 1e6, s->max_ns / 1e3, s->queue / 1e6, s->record / 1e6, s->wait / 1e6,
                    s->writeback / 1e6, s->image / 1e6, s->diag / 1e6);
        static const char* outcome_names[] = {"notmapped", "fast", "dl", "empty",
                                              "wnoflush", "imgwait", "skipped", "side"};
        for (u32 i = 0; i < NumOutcomes; ++i) {
            std::printf(" %s=%llu", outcome_names[i], (unsigned long long)s->outcome[i]);
        }
        std::printf(" downloads=%llu copies=%llu bytes=%llu changed=%llu hot_bytes=%llu "
                    "hot_changed=%llu copied=%llu unchanged=%llu hit=%llu page=%llu "
                    "st_unknown=%llu st_done=%llu st_submitted=%llu st_recording=%llu",
                    (unsigned long long)s->downloads, (unsigned long long)s->copies,
                    (unsigned long long)s->bytes, (unsigned long long)s->changed,
                    (unsigned long long)s->hot_bytes, (unsigned long long)s->hot_changed,
                    (unsigned long long)s->copied, (unsigned long long)s->unchanged,
                    (unsigned long long)s->hit, (unsigned long long)s->page,
                    (unsigned long long)s->state[0], (unsigned long long)s->state[1],
                    (unsigned long long)s->state[2], (unsigned long long)s->state[3]);
        if (s->age_n) {
            std::printf(" age_min_us=%.0f age_avg_us=%.0f", s->min_age / 1e3,
                        s->age_sum / 1e3 / s->age_n);
        }
        u32 label = NumLabelKinds;
        for (u32 k = 0; k <= NumLabelKinds; ++k) {
            if (s->label[k] > (label < NumLabelKinds + 1 ? s->label[label] : 0)) {
                label = k;
            }
        }
        std::printf(" label=%s label_age_us=%.0f last=%llx", LabelName(label),
                    s->n ? s->label_age_sum / 1e3 / s->n : 0.0, (unsigned long long)s->last_addr);
        std::vector<std::pair<u32, u64>> prods(s->producers.begin(), s->producers.end());
        std::ranges::sort(prods, [](const auto& a, const auto& b) { return a.second > b.second; });
        std::printf(" producers=");
        for (size_t i = 0; i < prods.size() && i < 4; ++i) {
            std::printf("%s%s/%llu", i ? "," : "", ProducerText(prods[i].first).c_str(),
                        (unsigned long long)prods[i].second);
        }
        std::printf("\n");
    }
    for (auto& [addr, im] : images) {
        if (!im.queued && !im.cpu_read && !im.cpu_write && !im.gpu && !im.skipped) {
            continue;
        }
        std::printf("RBD img addr=%llx w=%u h=%u bits=%u fmt=%u rt=%d size=%llu queued=%llu "
                    "bytes=%llu landed=%llu changed=%llu changed_copies=%llu skipped=%llu "
                    "cpu_r=%llu cpu_w=%llu gpu=%llu wait_ms=%.2f sites=",
                    (unsigned long long)addr, im.width, im.height, im.bits, im.format,
                    im.render_target, (unsigned long long)im.size, (unsigned long long)im.queued,
                    (unsigned long long)im.bytes, (unsigned long long)im.landed,
                    (unsigned long long)im.changed, (unsigned long long)im.changed_copies,
                    (unsigned long long)im.skipped, (unsigned long long)im.cpu_read,
                    (unsigned long long)im.cpu_write, (unsigned long long)im.gpu,
                    im.wait_ns / 1e6);
        size_t i = 0;
        for (const auto& [site, count] : im.sites) {
            if (i++ < 4) {
                std::printf("%s%s/%llu", i > 1 ? "," : "", Describe(site).c_str(),
                            (unsigned long long)count);
            }
        }
        std::printf("\n");
        const auto keep = im;
        im = {};
        im.width = keep.width, im.height = keep.height, im.bits = keep.bits;
        im.format = keep.format, im.render_target = keep.render_target, im.size = keep.size;
    }
    for (u32 i = 0; i < 16; ++i) {
        const Resolve& r = resolves[i];
        if (r.n) {
            std::printf("RBD imgresolve src=%s n=%llu pending=%llu waited=%llu ms=%.2f bytes=%llu\n",
                        SourceName(i), (unsigned long long)r.n, (unsigned long long)r.pending,
                        (unsigned long long)r.waited, r.ns / 1e6, (unsigned long long)r.bytes);
        }
        resolves[i] = {};
    }
    static const char* finish_names[] = {"budget", "too_large", "not_deferred"};
    for (u32 i = 0; i < 3; ++i) {
        if (finish_n[i]) {
            std::printf("RBD imgfinish reason=%s n=%llu ms=%.2f\n", finish_names[i],
                        (unsigned long long)finish_n[i], finish_ns[i] / 1e6);
        }
        finish_n[i] = finish_ns[i] = 0;
    }
    std::fflush(stdout);
    sites.clear();
    window_start = now;
}

void MaybePrint() {
    const u64 now = NowNs();
    u64 due = next_print.load(std::memory_order_relaxed);
    if (now < due || !next_print.compare_exchange_strong(due, now + WindowNs)) {
        return;
    }
    std::scoped_lock lock{mutex};
    if (window_start) {
        PrintWindow(now);
    }
    window_start = now;
}

void Record(Ctx& ctx) {
    const u64 now = NowNs();
    const u64 ns = now - ctx.t_fault;
    u64 key;
    if (ctx.internal) {
        key = 0x9e3779b97f4a7c15ull;
        for (const u64 f : ctx.frames) {
            key = (key ^ f) * 0x100000001b3ull;
        }
    } else {
        key = ctx.rip;
    }
    key = key * 2 + ctx.write;
    std::scoped_lock lock{mutex};
    Site& s = sites[key];
    if (!s.n) {
        s.rip = ctx.rip;
        s.write = ctx.write;
        s.internal = ctx.internal;
        std::memcpy(s.frames, ctx.frames, sizeof(s.frames));
    }
    s.caller = ctx.caller;
    s.last_addr = ctx.addr;
    ++s.n;
    s.ns += ns;
    s.max_ns = std::max(s.max_ns, ns);
    s.queue += ctx.queue_ns;
    s.record += ctx.record_ns;
    s.wait += ctx.wait_ns;
    s.writeback += ctx.writeback_ns;
    s.image += ctx.image_ns;
    s.diag += ctx.diag_ns;
    for (u32 i = 0; i < NumOutcomes; ++i) {
        s.outcome[i] += (ctx.outcome >> i) & 1;
    }
    s.downloads += ctx.downloads;
    s.copies += ctx.copies;
    s.bytes += ctx.bytes;
    s.changed += ctx.changed;
    s.hot_bytes += ctx.hot_bytes;
    s.hot_changed += ctx.hot_changed;
    if (ctx.bytes) {
        ++s.copied;
        s.unchanged += ctx.changed == 0;
        ++s.state[ctx.last_write_state & 3];
    }
    s.hit += ctx.changed_hit != 0;
    s.page += ctx.changed_page != 0;
    if (ctx.min_age_ns != ~0ull) {
        s.min_age = std::min(s.min_age, ctx.min_age_ns);
        s.age_sum += ctx.min_age_ns;
        ++s.age_n;
    }
    ++s.label[std::min<u32>(ctx.label_kind, NumLabelKinds)];
    s.label_age_sum += ctx.label_age_ns;
    for (int i = 0; i < Ctx::MaxProducers; ++i) {
        if (ctx.producer_changed[i]) {
            s.producers[ctx.producer[i]] += ctx.producer_changed[i];
        }
    }
    static const char* path = std::getenv("TLG_RB_TRACE");
    if (path && !trace) {
        trace = std::fopen(path, "w");
        if (trace) {
            std::fprintf(trace, "t_ms,rw,site,caller,addr,outcome,us,queue_us,wait_us,img_us,"
                                "bytes,changed,hit,page,producer\n");
        }
    }
    if (trace) {
        std::fprintf(trace, "%.3f,%c,%s,%s,%llx,%x,%.1f,%.1f,%.1f,%.1f,%llu,%llu,%llu,%llu,%s\n",
                     ctx.t_fault / 1e6, ctx.write ? 'W' : 'R',
                     ctx.internal ? Describe(ctx.frames[0]).c_str() : Describe(ctx.rip).c_str(),
                     Describe(ctx.caller).c_str(), (unsigned long long)ctx.addr, ctx.outcome,
                     ns / 1e3, ctx.queue_ns / 1e3, ctx.wait_ns / 1e3, ctx.image_ns / 1e3,
                     (unsigned long long)ctx.bytes, (unsigned long long)ctx.changed,
                     (unsigned long long)ctx.changed_hit, (unsigned long long)ctx.changed_page,
                     ctx.producer_changed[0] ? ProducerText(ctx.producer[0]).c_str() : "");
    }
}

void SnapshotLabels(Ctx& ctx) {
    u64 newest = 0;
    for (u32 k = 0; k < NumLabelKinds; ++k) {
        const u64 t = label_t[k].load(std::memory_order_relaxed);
        if (t > newest) {
            newest = t;
            ctx.label_kind = k;
        }
    }
    ctx.label_age_ns = newest && ctx.t_fault > newest ? ctx.t_fault - newest : 0;
}
} // namespace

bool Enabled() {
    static const bool enabled = [] {
        const char* e = std::getenv("TLG_RB_DIAG");
        return e && *e == '1';
    }();
    return enabled;
}

u64 NowNs() {
    return u64(std::chrono::duration_cast<std::chrono::nanoseconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
                   .count());
}

void BeginFault(Ctx& ctx, void* context, u64 addr, bool write, bool gpu_thread) {
    ReloadControl();
    ctx.addr = addr;
    ctx.write = write;
    ctx.gpu_thread = gpu_thread;
    ctx.t_fault = NowNs();
#ifdef _WIN32
    const CONTEXT* c = static_cast<EXCEPTION_POINTERS*>(context)->ContextRecord;
    ctx.rip = c->Rip;
    const u64 rsp = c->Rsp;
#else
    const auto* g = static_cast<ucontext_t*>(context)->uc_mcontext.gregs;
    ctx.rip = u64(g[REG_RIP]);
    const u64 rsp = u64(g[REG_RSP]);
#endif
    // The return address at the top of the stack (right for leaf routines such as memcpy).
    ctx.caller = *reinterpret_cast<const u64*>(rsp);
    SnapshotLabels(ctx);
    ctx.previous = current;
    current = &ctx;
}

void EndFault(Ctx& ctx, bool handled) {
    current = ctx.previous;
    if (!handled) {
        ctx.outcome |= NotMapped;
    }
    Record(ctx);
    MaybePrint();
}

Ctx* Current() {
    return current;
}

void AddOutcome(u32 outcome) {
    if (current) {
        current->outcome |= outcome;
    }
}

Scope::Scope(Ctx* ctx) : previous{current} {
    current = ctx;
}

Scope::~Scope() {
    current = previous;
}

InternalScope::InternalScope(u64 addr, bool write) {
    if (!Enabled() || current) {
        return;
    }
    active = true;
    ctx.internal = true;
    ctx.addr = addr;
    ctx.write = write;
    ctx.t_fault = NowNs();
#ifdef _WIN32
    void* frames[4]{};
    const USHORT n = RtlCaptureStackBackTrace(1, 4, frames, nullptr);
    for (USHORT i = 0; i < n; ++i) {
        ctx.frames[i] = reinterpret_cast<u64>(frames[i]);
    }
#else
    ctx.frames[0] = reinterpret_cast<u64>(__builtin_return_address(0));
#endif
    ctx.rip = ctx.frames[0];
    SnapshotLabels(ctx);
    ctx.previous = current;
    current = &ctx;
}

InternalScope::~InternalScope() {
    if (active) {
        current = ctx.previous;
        Record(ctx);
        MaybePrint();
    }
}

bool SkipBuffer(const Ctx& ctx) {
    if (!std::getenv("TLG_RB_CONTROL")) {
        return false;
    }
    ReloadControl();
    std::scoped_lock lock{control_mutex};
    if (ctx.write ? control.skipw : control.skipr) {
        return true;
    }
    const u64 site = GuestOffset(ctx.internal ? ctx.frames[0] : ctx.rip);
    if (!control.only.empty() && !control.only.contains(site)) {
        return true;
    }
    return control.skip.contains(site);
}

bool SkipImage(u64 image_addr) {
    if (!std::getenv("TLG_RB_CONTROL")) {
        return false;
    }
    ReloadControl();
    std::scoped_lock lock{control_mutex};
    if (!control.onlyimg.empty() && !control.onlyimg.contains(image_addr)) {
        return true;
    }
    return control.skipimg.contains(image_addr);
}

namespace {
thread_local bool fix_skip = false;

std::vector<u64> FixSites(const char* name) {
    std::vector<u64> sites;
    if (const char* e = std::getenv(name)) {
        for (char* end; *e; e = *end ? end + 1 : end) {
            const u64 site = std::strtoull(e, &end, 16);
            if (end == e) {
                break;
            }
            sites.push_back(site);
        }
    }
    return sites;
}
} // namespace

FixScope::FixScope(void* context, bool write) : previous{fix_skip} {
    static const std::vector<u64> read_sites = FixSites("TLG_RB_SKIP_READ");
    static const std::vector<u64> write_sites = FixSites("TLG_RB_SKIP_WRITE");
    const auto& sites = write ? write_sites : read_sites;
    if (sites.empty()) {
        return;
    }
#ifdef _WIN32
    const u64 rip = static_cast<EXCEPTION_POINTERS*>(context)->ContextRecord->Rip;
#else
    const u64 rip = u64(static_cast<ucontext_t*>(context)->uc_mcontext.gregs[REG_RIP]);
#endif
    fix_skip = std::ranges::find(sites, GuestOffset(rip)) != sites.end();
}

FixScope::~FixScope() {
    fix_skip = previous;
}

bool FixSkip() {
    return fix_skip;
}

bool FixSkipImage(u64 size) {
    static const u64 max = [] {
        const char* e = std::getenv("TLG_RB_IMG_MAX_BYTES");
        return e ? std::strtoull(e, nullptr, 10) : 0ull;
    }();
    return max && size > max;
}

ProducerScope::ProducerScope(ProducerKind kind, u64 hash) : previous{current_producer} {
    current_producer = u64(kind) << 56 | (hash & ((1ull << 56) - 1));
}

ProducerScope::~ProducerScope() {
    current_producer = previous;
}

void NoteGpuWrite(u64 addr, u64 size, const void* return_address) {
    if (!size) {
        return;
    }
    const u64 p = current_producer
                      ? current_producer
                      : u64(ProdOther) << 56 | reinterpret_cast<u64>(return_address);
    const u64 now = NowNs();
    std::scoped_lock lock{write_mutex};
    const u32 id = ProducerId(p);
    const u64 first = addr >> GranuleShift;
    const u64 last = std::min((addr + size - 1) >> GranuleShift, first + 4095);
    for (u64 g = first; g <= last; ++g) {
        Granule& granule = writes[g];
        granule.rec[granule.next++ & 3] = {addr, addr + size, now, id};
    }
}

void NoteCopy(u64 dst_addr, const void* downloaded, u64 size, bool hot) {
    Ctx* ctx = current;
    if (!ctx || !size) {
        return;
    }
    const u64 t0 = NowNs();
    thread_local std::vector<unsigned char> guest;
    guest.resize(size);
    if (!runtime_memory_read_backing(dst_addr, guest.data(), size)) {
        // Unknown: count every byte as changed (a direct read would fault on protected pages).
        std::memset(guest.data(), 0, size);
        for (u64 i = 0; i < size; ++i) {
            guest[i] = static_cast<const unsigned char*>(downloaded)[i] ^ 0xff;
        }
    }
    const auto* gpu = static_cast<const unsigned char*>(downloaded);
    u64 changed = 0;
    const u64 hit_lo = ctx->addr, hit_hi = ctx->addr + 8;
    const u64 page_lo = ctx->addr & ~4095ull, page_hi = page_lo + 4096;
    std::scoped_lock lock{write_mutex};
    for (u64 chunk = 0; chunk < size; chunk += 64) {
        const u64 n = std::min<u64>(64, size - chunk);
        if (!std::memcmp(gpu + chunk, guest.data() + chunk, n)) {
            continue;
        }
        u64 chunk_changed = 0;
        for (u64 i = chunk; i < chunk + n; ++i) {
            if (gpu[i] != guest[i]) {
                ++chunk_changed;
                const u64 at = dst_addr + i;
                ctx->changed_hit += at >= hit_lo && at < hit_hi;
                ctx->changed_page += at >= page_lo && at < page_hi;
            }
        }
        changed += chunk_changed;
        // The youngest recorded GPU write covering this chunk.
        const u64 at = dst_addr + chunk;
        u32 producer = 0;
        u64 t = 0;
        if (const auto it = writes.find(at >> GranuleShift); it != writes.end()) {
            for (const WriteRec& rec : it->second.rec) {
                if (rec.t > t && at < rec.end && at + n > rec.start) {
                    t = rec.t;
                    producer = rec.producer;
                }
            }
        }
        if (t) {
            ctx->min_age_ns = std::min(ctx->min_age_ns, t0 > t ? t0 - t : 0);
        }
        int slot = Ctx::MaxProducers - 1;
        for (int i = 0; i < Ctx::MaxProducers; ++i) {
            if (ctx->producer[i] == producer || !ctx->producer_changed[i]) {
                slot = i;
                break;
            }
        }
        ctx->producer[slot] = producer;
        ctx->producer_changed[slot] += chunk_changed;
    }
    ++ctx->copies;
    if (hot) {
        ctx->hot_bytes += size;
        ctx->hot_changed += changed;
    } else {
        ctx->bytes += size;
        ctx->changed += changed;
    }
    ctx->diag_ns += NowNs() - t0;
}

void NoteDownload(u64 record_ns, u64 wait_ns, u64 writeback_ns, u32 state, bool side,
                  bool empty) {
    Ctx* ctx = current;
    if (!ctx) {
        return;
    }
    ++ctx->downloads;
    ctx->outcome |= Download | (side ? Side : 0) | (empty ? Empty : 0);
    ctx->record_ns += record_ns;
    ctx->wait_ns += wait_ns;
    ctx->writeback_ns += writeback_ns;
    if (!empty) {
        ctx->last_write_state = std::max(ctx->last_write_state, state);
    }
}

void NoteImageQueue(u64 addr, u32 width, u32 height, u32 bits, u32 format, bool render_target,
                    u64 size) {
    std::scoped_lock lock{mutex};
    Image& im = images[addr];
    im.width = width, im.height = height, im.bits = bits, im.format = format;
    im.render_target = render_target;
    im.size = size;
    ++im.queued;
    im.bytes += size;
}

void NoteImageLand(u64 addr, const void* staging, u64 size) {
    thread_local std::vector<unsigned char> guest;
    guest.resize(size);
    if (!runtime_memory_read_backing(addr, guest.data(), size)) {
        return;
    }
    const auto* gpu = static_cast<const unsigned char*>(staging);
    u64 changed = 0;
    for (u64 i = 0; i < size; ++i) {
        changed += gpu[i] != guest[i];
    }
    std::scoped_lock lock{mutex};
    Image& im = images[addr];
    ++im.landed;
    im.changed += changed;
    im.changed_copies += changed != 0;
}

void NoteImageResolve(Ctx* ctx, u32 source, u64 addr, u64 size, bool pending, bool waited,
                      u64 ns) {
    if (ctx) {
        ctx->image_ns += ns;
        if (waited) {
            ctx->outcome |= ImageWait;
        }
    }
    std::scoped_lock lock{mutex};
    Resolve& r = resolves[source & 15];
    ++r.n;
    r.pending += pending;
    r.waited += waited;
    r.ns += ns;
    r.bytes += size;
    if (!pending) {
        return;
    }
    for (auto& [image_addr, im] : images) {
        if (addr < image_addr + im.size && image_addr < addr + size) {
            if (!ctx) {
                ++im.gpu;
            } else if (ctx->write) {
                ++im.cpu_write;
            } else {
                ++im.cpu_read;
            }
            im.wait_ns += ns;
            if (ctx) {
                ++im.sites[ctx->internal ? ctx->frames[0] : ctx->rip];
            }
        }
    }
}

void NoteImageFinish(u32 reason, u64 ns) {
    std::scoped_lock lock{mutex};
    ++finish_n[std::min<u32>(reason, 2)];
    finish_ns[std::min<u32>(reason, 2)] += ns;
}

void NoteImageSkip(u64 addr) {
    std::scoped_lock lock{mutex};
    ++images[addr].skipped;
}

void NoteLabel(LabelKind kind, u64 addr) {
    (void)addr;
    labels[kind].fetch_add(1, std::memory_order_relaxed);
    label_t[kind].store(NowNs(), std::memory_order_relaxed);
    MaybePrint();
}
} // namespace TlgRbDiag
