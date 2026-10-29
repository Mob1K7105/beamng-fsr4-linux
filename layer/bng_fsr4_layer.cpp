// Vulkan layer for native-Linux BeamNG.drive: replaces the output of the game's FSR 3 "custom" upscaler with
// FSR 4.1.1 at the RCAS dispatch and skips FSR 3's own passes. Every identification or consistency check
// fails open to the game's FSR 3.
//
// Players turn it on with BNG_FSR4=1 (the release manifest's enable_environment). The FSR4STUDY_* variables
// are the development switches; FSR4STUDY_DIAG=1 adds the diagnostic capture and logging paths.
#include <vulkan/vulkan.h>
#include <vulkan/vk_layer.h>
#include "fsr411.h"
#include <array>
#include <algorithm>
#include <memory>

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>
#include <dlfcn.h>
#include <ctime>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace {

// ---- release switch ------------------------------------------------------------------------------
// BNG_FSR4=1 (the release manifest's enable_environment) is the only variable a player sets. It fills
// in the study variables before any static initializer reads them: FSR 4 on, assets next to the
// library, log under $XDG_CACHE_HOME/bng-fsr4. In any process other than BeamNG the layer stays inert.
bool g_release = false, g_inert = false;
__attribute__((constructor(101))) void ReleaseDefaults() {
    if (!getenv("BNG_FSR4") || !strcmp(getenv("BNG_FSR4"), "0")) return;
    g_release = true;
    char exe[512] = {}; if (readlink("/proc/self/exe", exe, sizeof exe - 1) < 0) exe[0] = 0;
    if (!strstr(exe, "BeamNG.drive")) { g_inert = true; return; }
    setenv("FSR4STUDY_FSR4", "1", 0);
    Dl_info info{};
    if (dladdr(reinterpret_cast<void*>(&ReleaseDefaults), &info) && info.dli_fname) {
        std::string dir = info.dli_fname; dir = dir.substr(0, dir.find_last_of('/'));
        setenv("FSR4STUDY_ASSETS", (dir + "/assets").c_str(), 0);
    }
    const char* cache = getenv("XDG_CACHE_HOME"); const char* home = getenv("HOME");
    std::string base = cache && *cache ? cache : std::string(home ? home : "/tmp") + "/.cache";
    mkdir(base.c_str(), 0755);
    setenv("FSR4STUDY_OUT", (base + "/bng-fsr4").c_str(), 0);
    if (const char* v = getenv("BNG_FSR4_SHARPNESS")) setenv("FSR4STUDY_SHARPNESS", v, 0);
    if (getenv("BNG_FSR4_NO_SHARPEN")) setenv("FSR4STUDY_NO_SHARPEN", "1", 0);
    if (getenv("BNG_FSR4_KEEP_FSR3")) setenv("FSR4STUDY_KEEP_FSR3", "1", 0);
    if (const char* v = getenv("BNG_FSR4_VARIANT")) setenv("FSR4STUDY_VARIANT", v, 0);
}

std::mutex g_mutex;
FILE* g_events = nullptr;  // creation events, always on
FILE* g_frames = nullptr;  // command stream, capture windows only
std::string g_out;
std::atomic<uint64_t> g_frame{0};
std::atomic<int> g_capture_left{0};  // frames still to capture
std::atomic<bool> g_capturing{false};
uint64_t g_capture_start = UINT64_MAX;
int g_capture_count = 2;
size_t g_frames_bytes = 0;
constexpr size_t kMaxFrameLog = 300u << 20;

uint64_t Tid() { return uint64_t(syscall(SYS_gettid)) & 0xffff; }

void Ev(const char* fmt, ...) {
    std::lock_guard<std::mutex> l(g_mutex);
    if (!g_events) return;
    va_list a; va_start(a, fmt); vfprintf(g_events, fmt, a); va_end(a);
    fputc('\n', g_events);
    fflush(g_events);
}
void Fr(const char* fmt, ...) {
    if (!g_capturing) return;
    std::lock_guard<std::mutex> l(g_mutex);
    if (!g_frames || g_frames_bytes > kMaxFrameLog) return;
    va_list a; va_start(a, fmt);
    char buf[4096];
    int n = vsnprintf(buf, sizeof buf, fmt, a); va_end(a);
    if (n < 0) return;
    fprintf(g_frames, "f%llu t%llx %s\n", (unsigned long long)g_frame.load(), (unsigned long long)Tid(), buf);
    g_frames_bytes += size_t(n) + 24;
}

// ---- object tables -------------------------------------------------------------------------------
struct ImageInfo { VkFormat fmt; uint32_t w, h, d, layers, mips; VkImageUsageFlags usage; std::string name; };
struct ViewInfo { uint64_t image; VkFormat fmt; std::string name; int type = 0; uint32_t aspect = 0, base_mip = 0, mips = 0, base_layer = 0, layers = 0; uint32_t swz = 0; };
// Upscale-chain role of a compute pipeline, classified once at creation (not per dispatch).
enum : uint32_t { kPfUpscale = 1, kPfPrepare = 2, kPfAccumulate = 4, kPfRcas = 8, kPfSkippable = 16 };
struct PipeInfo { bool compute; uint64_t module; uint64_t layout; std::string name; std::string module_name; uint64_t module_hash = 0; uint32_t flags = 0; };
uint32_t ClassifyModule(const std::string& mn) {
    uint32_t f = 0;
    if (mn.find("/postFx/upscale/") != std::string::npos) f |= kPfUpscale;
    if (mn.find("fsr3PrepareInputs.cs.hlsl") != std::string::npos) f |= kPfPrepare;
    if (mn.find("fsr3Accumulate.cs.hlsl") != std::string::npos) f |= kPfAccumulate;
    if (mn.find("fsr3Rcas.cs.hlsl") != std::string::npos) f |= kPfRcas;
    if (mn.find("/upscale/fsr3/") != std::string::npos && !(f & kPfRcas) && mn.find("fsr3DebugView.cs.hlsl") == std::string::npos) f |= kPfSkippable;
    // The engine's reactive mask for FSR 3 (FSR 4 takes no reactive input); its mask history goes stale while
    // skipped, which the 32-frame handback to FSR 3 absorbs.
    if (mn.find("/upscale/dynamicDisocclusion.cs.hlsl") != std::string::npos) f |= kPfSkippable;
    return f;
}
struct CbInfo { uint64_t compute_pipe = 0; uint64_t gfx_pipe = 0; uint32_t draws = 0; uint64_t set0 = 0; };
struct Desc { int type; uint64_t view_or_buf; uint64_t sampler; uint64_t off, range; int layout; };
struct BufInfo { uint64_t size = 0, mem = 0, off = 0; };
std::map<uint64_t, std::map<uint64_t, Desc>> g_dsets;  // set -> (binding<<32|elem) -> descriptor
std::map<uint64_t, BufInfo> g_bufs;
std::map<uint64_t, char*> g_mapped;                     // VkDeviceMemory -> host base pointer
std::atomic<uint64_t> g_template_updates{0};
std::map<uint64_t, std::vector<uint8_t>> g_bufdata;  // small buffers: last uploaded contents (record order)
std::map<std::pair<uint64_t, uint64_t>, std::vector<uint8_t>> g_memdata;  // (memory, absolute offset) -> last small upload
VkDevice g_device = VK_NULL_HANDLE; VkPhysicalDevice g_pd = VK_NULL_HANDLE; VkInstance g_instance = VK_NULL_HANDLE;
PFN_vkGetInstanceProcAddr g_gipa = nullptr;
std::map<uint64_t, std::string> g_modname;  // VkShaderModule -> debug name (HLSL path)
// Lean mode (default with FSR4STUDY_FSR4; FSR4STUDY_DIAG=1 turns it off): expose only the hooks FSR 4 needs,
// no capture, no buffer shadowing, descriptors shadowed only for the bindings FSR 4 reads, and the control
// files polled every 8th present instead of several times per frame.
const bool g_lean = getenv("FSR4STUDY_FSR4") != nullptr && getenv("FSR4STUDY_DIAG") == nullptr;
constexpr int kRbSlots = 16, kRbBytes = 256;
VkBuffer g_rb = VK_NULL_HANDLE; uint8_t* g_rb_ptr = nullptr; uint64_t g_rb_frame[kRbSlots] = {}; FILE* g_rb_log = nullptr;
// --- frame dump (inputs before PrepareInputs, FSR3 output after RCAS), armed by dump.trigger ---
struct DumpPart { const char* name; uint64_t off = 0, bytes = 0; uint32_t w = 0, h = 0; int fmt = 0; };
struct DumpSlot { uint64_t frame = 0; bool in_ok = false, out_ok = false; DumpPart color{"color"}, depth{"depth"}, mv{"mv"}, expo{"exposure"}, out{"output"}, rcasin{"rcasin"}; };
int g_dump_total = 12; int g_dump_left = 0; int g_dump_next = 0; std::vector<DumpSlot> g_dump; uint64_t g_dump_slot_bytes = 0;
VkBuffer g_dump_buf = VK_NULL_HANDLE; uint8_t* g_dump_ptr = nullptr; uint64_t g_dump_cap = 0; std::atomic<bool> g_dump_active{false};
struct DumpAllocation { VkBuffer buffer; VkDeviceMemory memory; uint8_t* mapped; uint64_t capacity; };
std::vector<DumpAllocation> g_dump_allocations;
bool g_fsr4_features = false, g_fsr4_valve = false, g_fsr4_wave = false;
std::atomic<uint64_t> g_upd_cnt{0}, g_copy_cnt{0}, g_copy_mapped{0};
std::map<uint64_t, ImageInfo> g_images;
std::map<uint64_t, ViewInfo> g_views;
std::map<uint64_t, PipeInfo> g_pipes;
std::map<uint64_t, std::string> g_names;   // everything else, by handle
std::map<uint64_t, uint64_t> g_module_hash; // VkShaderModule -> fnv
std::map<uint64_t, CbInfo> g_cbs;
std::set<uint64_t> g_push_layouts;
// Opt-in: snapshot the GPU-visible constant descriptors of one backend frame.
struct UniformProbePart { uint32_t pass, binding; VkDeviceSize offset; std::vector<uint8_t> expected; };
struct UniformProbe {
    VkBuffer buffer = VK_NULL_HANDLE; uint8_t* mapped = nullptr;
    VkDeviceSize cursor = 0; uint64_t counter = 0; bool recorded = false, checked = false;
    std::vector<UniformProbePart> parts;
} g_uniform_probe;
thread_local bool g_uniform_probe_active = false;
thread_local uint32_t g_uniform_probe_pass = 0;
thread_local std::map<uint32_t, VkDescriptorBufferInfo> g_uniform_descriptors;
void ProbeUniforms(VkCommandBuffer c);
void BeginUniformProbe();
void FinishUniformProbe();

const char* LayoutNameFwd(int l);
std::string Hex(uint64_t v) { char b[24]; snprintf(b, sizeof b, "%llx", (unsigned long long)v); return b; }

std::string ImageStr(uint64_t h) {
    std::lock_guard<std::mutex> l(g_mutex);
    auto it = g_images.find(h);
    if (it == g_images.end()) return "img:" + Hex(h) + "[?]";
    const auto& i = it->second;
    char b[200];
    snprintf(b, sizeof b, "img:%s[%s fmt%d %ux%ux%u L%u M%u use%x]", i.name.empty() ? Hex(h).c_str() : i.name.c_str(),
             Hex(h).c_str(), int(i.fmt), i.w, i.h, i.d, i.layers, i.mips, unsigned(i.usage));
    return b;
}
std::string ViewStr(uint64_t h) {
    uint64_t img = 0; std::string vn;
    {
        std::lock_guard<std::mutex> l(g_mutex);
        auto it = g_views.find(h);
        if (it == g_views.end()) return "view:" + Hex(h) + "[?]";
        img = it->second.image; vn = it->second.name;
    }
    return "view:" + (vn.empty() ? Hex(h) : vn) + "->" + ImageStr(img);
}
std::string PipeStr(uint64_t h) {
    std::lock_guard<std::mutex> l(g_mutex);
    auto it = g_pipes.find(h);
    if (it == g_pipes.end()) return "pipe:" + Hex(h) + "[?]";
    auto m = g_module_hash.find(it->second.module);
    char b[160];
    snprintf(b, sizeof b, "pipe:%s%s %s mod=%s", Hex(h).c_str(), it->second.compute ? "(C)" : "(G)",
             it->second.name.c_str(), m == g_module_hash.end() ? "?" : Hex(m->second).c_str());
    return b;
}

uint64_t Fnv(const void* p, size_t n) {
    const uint8_t* b = static_cast<const uint8_t*>(p); uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < n; ++i) { h ^= b[i]; h *= 1099511628211ull; }
    return h;
}

// Minimal SPIR-V scan: stage, local size, capabilities, extensions, (set,binding) list.
std::string ScanSpirv(const uint32_t* w, size_t words, bool* is_compute) {
    *is_compute = false;
    if (words < 5 || w[0] != 0x07230203) return "not-spirv";
    uint32_t lsz[3] = {0, 0, 0}; std::vector<uint32_t> caps; std::string exts;
    std::map<uint32_t, std::pair<int, int>> sb;  // id -> (set, binding)
    std::map<uint32_t, std::string> nm;          // id -> OpName
    for (size_t i = 5; i < words;) {
        uint32_t op = w[i] & 0xffff, len = w[i] >> 16;
        if (!len || i + len > words) break;
        if (op == 15 && w[i + 1] == 5) *is_compute = true;
        if (op == 16 && w[i + 2] == 17) { lsz[0] = w[i + 3]; lsz[1] = w[i + 4]; lsz[2] = w[i + 5]; }
        if (op == 17) caps.push_back(w[i + 1]);
        if (op == 10) { exts += reinterpret_cast<const char*>(&w[i + 1]); exts += ' '; }
        if (op == 5) nm[w[i + 1]] = reinterpret_cast<const char*>(&w[i + 2]);
        if (op == 71 && w[i + 2] == 34) sb[w[i + 1]].first = int(w[i + 3]);
        if (op == 71 && w[i + 2] == 33) sb[w[i + 1]].second = int(w[i + 3]);
        i += len;
    }
    std::string s = std::string(*is_compute ? "COMPUTE" : "other") + " local=" + std::to_string(lsz[0]) + "x" +
                    std::to_string(lsz[1]) + "x" + std::to_string(lsz[2]) + " caps=";
    for (auto c : caps) s += std::to_string(c) + ",";
    s += " exts=" + exts + " set:binding=";
    std::set<std::pair<int, int>> uniq;
    for (auto& kv : sb) uniq.insert(kv.second);
    for (auto& p : uniq) s += std::to_string(p.first) + ":" + std::to_string(p.second) + ",";
    s += " names=";
    for (auto& kv : sb) if (nm.count(kv.first)) s += std::to_string(kv.second.second) + "=" + nm[kv.first] + ",";
    return s;
}

// ---- dispatch tables -----------------------------------------------------------------------------
#define DEV_FUNCS(X) \
    X(GetDeviceProcAddr) X(DestroyDevice) X(CreateShaderModule) X(CreateComputePipelines) X(CreateGraphicsPipelines) \
    X(CreateImage) X(CreateImageView) X(CreatePipelineLayout) X(CreateDescriptorSetLayout) X(SetDebugUtilsObjectNameEXT) \
    X(CmdBeginDebugUtilsLabelEXT) X(CmdEndDebugUtilsLabelEXT) X(CmdInsertDebugUtilsLabelEXT) X(BeginCommandBuffer) \
    X(CmdBindPipeline) X(CmdDispatch) X(CmdDispatchIndirect) X(CmdPushConstants) X(CmdPushDescriptorSetKHR) \
    X(CmdBindDescriptorSets) X(UpdateDescriptorSets) X(CmdPipelineBarrier) X(CmdPipelineBarrier2) X(CmdBeginRendering) \
    X(CmdBeginRenderPass) X(CmdDraw) X(CmdDrawIndexed) X(CmdDrawIndirect) X(CmdDrawIndexedIndirect) \
    X(CmdDrawIndirectCount) X(CmdDrawIndexedIndirectCount) X(QueueSubmit) X(QueueSubmit2) X(QueuePresentKHR) \
    X(CmdCopyImage) X(CmdBlitImage) X(CmdCopyBufferToImage) X(CmdClearColorImage) X(CreateBuffer) X(DeviceWaitIdle) X(GetImageMemoryRequirements) X(BindImageMemory) X(GetBufferMemoryRequirements) X(AllocateMemory) X(BindBufferMemory) X(BindBufferMemory2) X(MapMemory) X(MapMemory2) X(CmdCopyImageToBuffer) X(CmdUpdateBuffer) X(CmdCopyBuffer) X(CmdCopyBuffer2) X(UpdateDescriptorSetWithTemplate)
struct Dev {
#define X(n) PFN_vk##n n = nullptr;
    DEV_FUNCS(X)
#undef X
    PFN_vkGetDeviceProcAddr gdpa = nullptr;
};
struct Inst { PFN_vkGetInstanceProcAddr gipa = nullptr; PFN_vkDestroyInstance destroy = nullptr; };
std::map<void*, Dev*> g_devs;
std::map<void*, Inst*> g_insts;
void* Key(void* h) { return *reinterpret_cast<void**>(h); }
std::atomic<uint32_t> g_devs_gen{0};  // bumped whenever g_devs changes; invalidates the per-thread cache
Dev* D(void* h) {
    thread_local void* cached_key = nullptr; thread_local Dev* cached = nullptr; thread_local uint32_t cached_gen = ~0u;
    void* key = Key(h); const uint32_t gen = g_devs_gen.load(std::memory_order_acquire);
    if (key == cached_key && gen == cached_gen) return cached;
    std::lock_guard<std::mutex> l(g_mutex); auto it = g_devs.find(key); Dev* dev = it == g_devs.end() ? nullptr : it->second;
    if (dev) { cached_key = key; cached = dev; cached_gen = gen; }
    return dev;
}

void MaybeStartCapture() {
    struct stat st;
    std::string trig = g_out + "/capture.trigger";
    if (g_capture_left == 0 && !g_capturing && (stat(trig.c_str(), &st) == 0 || g_frame == g_capture_start)) {
        unlink(trig.c_str());
        g_capture_left = g_capture_count;
        g_capturing = true;
        Ev("CAPTURE start at frame %llu for %d frames", (unsigned long long)g_frame.load(), g_capture_count);
    }
}

// ---- intercepted functions -----------------------------------------------------------------------
VKAPI_ATTR VkResult VKAPI_CALL L_CreateShaderModule(VkDevice d, const VkShaderModuleCreateInfo* ci, const VkAllocationCallbacks* a, VkShaderModule* m) {
    VkResult r = D(d)->CreateShaderModule(d, ci, a, m);
    if (r == VK_SUCCESS) {
        uint64_t h = Fnv(ci->pCode, ci->codeSize);
        { std::lock_guard<std::mutex> l(g_mutex); g_module_hash[(uint64_t)*m] = h; }
        if (g_lean) return r;  // the hash is all FSR 4 needs; skip the reflection log and the SPIR-V copy
        bool comp; std::string s = ScanSpirv(ci->pCode, ci->codeSize / 4, &comp);
        Ev("SHADER module=%llx hash=%016llx bytes=%zu %s", (unsigned long long)*m, (unsigned long long)h, ci->codeSize, s.c_str());
        if (comp) {
            char p[512]; snprintf(p, sizeof p, "%s/spv/%016llx.spv", g_out.c_str(), (unsigned long long)h);
            if (FILE* f = fopen(p, "wb")) { fwrite(ci->pCode, 1, ci->codeSize, f); fclose(f); }
        }
    }
    return r;
}
VKAPI_ATTR VkResult VKAPI_CALL L_CreateComputePipelines(VkDevice d, VkPipelineCache c, uint32_t n, const VkComputePipelineCreateInfo* ci, const VkAllocationCallbacks* a, VkPipeline* p) {
    VkResult r = D(d)->CreateComputePipelines(d, c, n, ci, a, p);
    for (uint32_t i = 0; r == VK_SUCCESS && i < n; ++i) {
        { std::lock_guard<std::mutex> l(g_mutex); auto module = (uint64_t)ci[i].stage.module;
          const std::string& mn = g_modname[module];
          g_pipes[(uint64_t)p[i]] = {true, module, (uint64_t)ci[i].layout, "", mn, g_module_hash[module], ClassifyModule(mn)}; }
        if (g_lean) continue;
        Ev("PIPELINE(C) %llx layout=%llx %s entry=%s flags=%x", (unsigned long long)p[i], (unsigned long long)ci[i].layout,
           PipeStr((uint64_t)p[i]).c_str(), ci[i].stage.pName, unsigned(ci[i].flags));
    }
    return r;
}
VKAPI_ATTR VkResult VKAPI_CALL L_CreateGraphicsPipelines(VkDevice d, VkPipelineCache c, uint32_t n, const VkGraphicsPipelineCreateInfo* ci, const VkAllocationCallbacks* a, VkPipeline* p) {
    VkResult r = D(d)->CreateGraphicsPipelines(d, c, n, ci, a, p);
    for (uint32_t i = 0; r == VK_SUCCESS && i < n; ++i) {
        std::lock_guard<std::mutex> l(g_mutex);
        g_pipes[(uint64_t)p[i]] = {false, ci[i].stageCount ? (uint64_t)ci[i].pStages[0].module : 0, (uint64_t)ci[i].layout, ""};
    }
    return r;
}
VKAPI_ATTR VkResult VKAPI_CALL L_CreateImage(VkDevice d, const VkImageCreateInfo* ci, const VkAllocationCallbacks* a, VkImage* im) {
    VkResult r = D(d)->CreateImage(d, ci, a, im);
    if (r == VK_SUCCESS) {
        { std::lock_guard<std::mutex> l(g_mutex); g_images[(uint64_t)*im] = {ci->format, ci->extent.width, ci->extent.height, ci->extent.depth, ci->arrayLayers, ci->mipLevels, ci->usage, ""}; }
        // The engine creates transient images all the time: a flushed log line each is a syscall on its render thread.
        if (!g_lean) Ev("IMAGE %llx fmt=%d %ux%ux%u layers=%u mips=%u usage=%x samples=%d", (unsigned long long)*im, int(ci->format), ci->extent.width, ci->extent.height, ci->extent.depth, ci->arrayLayers, ci->mipLevels, unsigned(ci->usage), int(ci->samples));
    }
    return r;
}
VKAPI_ATTR VkResult VKAPI_CALL L_CreateImageView(VkDevice d, const VkImageViewCreateInfo* ci, const VkAllocationCallbacks* a, VkImageView* v) {
    VkResult r = D(d)->CreateImageView(d, ci, a, v);
    if (r == VK_SUCCESS) { std::lock_guard<std::mutex> l(g_mutex); ViewInfo vi{(uint64_t)ci->image, ci->format, ""}; vi.type = int(ci->viewType); vi.aspect = ci->subresourceRange.aspectMask; vi.base_mip = ci->subresourceRange.baseMipLevel; vi.mips = ci->subresourceRange.levelCount; vi.base_layer = ci->subresourceRange.baseArrayLayer; vi.layers = ci->subresourceRange.layerCount; vi.swz = uint32_t(ci->components.r) | uint32_t(ci->components.g) << 8 | uint32_t(ci->components.b) << 16 | uint32_t(ci->components.a) << 24; g_views[(uint64_t)*v] = vi; }
    return r;
}
void ShadowStore(uint64_t dst, uint64_t off, const void* src, uint64_t n) {
    std::lock_guard<std::mutex> l(g_mutex);
    auto it = g_bufs.find(dst);
    if (it == g_bufs.end() || it->second.size > (1u << 20) || off + n > it->second.size) return;
    auto& v = g_bufdata[dst];
    if (v.size() < it->second.size) v.resize(it->second.size, 0);
    memcpy(v.data() + off, src, n);
}
VKAPI_ATTR void VKAPI_CALL L_CmdUpdateBuffer(VkCommandBuffer c, VkBuffer b, VkDeviceSize o, VkDeviceSize n, const void* data) {
    g_upd_cnt++; ShadowStore((uint64_t)b, o, data, n);
    Fr("cb%llx UPDATE_BUFFER dst=%llx off=%llu size=%llu", (unsigned long long)c, (unsigned long long)b, (unsigned long long)o, (unsigned long long)n);
    D(c)->CmdUpdateBuffer(c, b, o, n, data);
}
void CopyShadow(uint64_t src, uint64_t dst, uint64_t so, uint64_t dof, uint64_t n) {
    char* base = nullptr; BufInfo sb;
    { std::lock_guard<std::mutex> l(g_mutex); auto it = g_bufs.find(src); if (it != g_bufs.end()) { sb = it->second; auto m = g_mapped.find(sb.mem); if (m != g_mapped.end()) base = m->second; } }
    g_copy_cnt++;
    if (g_capturing) { BufInfo db; { std::lock_guard<std::mutex> l(g_mutex); auto it = g_bufs.find(dst); if (it != g_bufs.end()) db = it->second; }
        if (db.size && db.size <= 4096) Fr("COPYBUF src=%llx(%s) dst=%llx(size %llu mem=%llx+%llu) srcoff=%llu dstoff=%llu n=%llu", (unsigned long long)src, base ? "mapped" : "unmapped", (unsigned long long)dst, (unsigned long long)db.size, (unsigned long long)db.mem, (unsigned long long)db.off, (unsigned long long)so, (unsigned long long)dof, (unsigned long long)n); }
    if (base) {
        g_copy_mapped++; ShadowStore(dst, dof, base + sb.off + so, n);
        if (n <= (1u << 20)) { std::lock_guard<std::mutex> l(g_mutex); auto it = g_bufs.find(dst); if (it != g_bufs.end() && it->second.mem) {
            const uint8_t* p = reinterpret_cast<const uint8_t*>(base + sb.off + so);
            g_memdata[{it->second.mem, it->second.off + dof}].assign(p, p + n); } }
    }
}
VKAPI_ATTR void VKAPI_CALL L_CmdCopyBuffer(VkCommandBuffer c, VkBuffer s, VkBuffer d, uint32_t n, const VkBufferCopy* r) {
    for (uint32_t i = 0; i < n; ++i) CopyShadow((uint64_t)s, (uint64_t)d, r[i].srcOffset, r[i].dstOffset, r[i].size);
    D(c)->CmdCopyBuffer(c, s, d, n, r);
}
VKAPI_ATTR void VKAPI_CALL L_CmdCopyBuffer2(VkCommandBuffer c, const VkCopyBufferInfo2* i) {
    for (uint32_t k = 0; k < i->regionCount; ++k) CopyShadow((uint64_t)i->srcBuffer, (uint64_t)i->dstBuffer, i->pRegions[k].srcOffset, i->pRegions[k].dstOffset, i->pRegions[k].size);
    D(c)->CmdCopyBuffer2(c, i);
}
VKAPI_ATTR VkResult VKAPI_CALL L_AllocateMemory(VkDevice d, const VkMemoryAllocateInfo* ai, const VkAllocationCallbacks* a, VkDeviceMemory* m) {
    VkResult r = D(d)->AllocateMemory(d, ai, a, m);
    if (r == VK_SUCCESS && ai->allocationSize >= (1u << 20)) Ev("ALLOC mem=%llx size=%llu typeIndex=%u", (unsigned long long)*m, (unsigned long long)ai->allocationSize, ai->memoryTypeIndex);
    return r;
}
VKAPI_ATTR VkResult VKAPI_CALL L_CreateBuffer(VkDevice d, const VkBufferCreateInfo* ci, const VkAllocationCallbacks* a, VkBuffer* b) {
    // Readback is injected only in these modes. Make the engine's UBO copies
    // legal without changing the caller's create info or any installation.
    auto copy = *ci;
    if ((getenv("FSR4STUDY_FSR4") || getenv("FSR4STUDY_CB_READBACK")) &&
        (copy.usage & VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT)) copy.usage |= VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    VkResult r = D(d)->CreateBuffer(d, &copy, a, b);
    if (r == VK_SUCCESS) { std::lock_guard<std::mutex> l(g_mutex); g_bufs[(uint64_t)*b].size = ci->size; }
    return r;
}
VKAPI_ATTR VkResult VKAPI_CALL L_BindBufferMemory(VkDevice d, VkBuffer b, VkDeviceMemory m, VkDeviceSize o) {
    { std::lock_guard<std::mutex> l(g_mutex); g_bufs[(uint64_t)b].mem = (uint64_t)m; g_bufs[(uint64_t)b].off = o; }
    return D(d)->BindBufferMemory(d, b, m, o);
}
VKAPI_ATTR VkResult VKAPI_CALL L_BindBufferMemory2(VkDevice d, uint32_t n, const VkBindBufferMemoryInfo* i) {
    { std::lock_guard<std::mutex> l(g_mutex); for (uint32_t k = 0; k < n; ++k) { g_bufs[(uint64_t)i[k].buffer].mem = (uint64_t)i[k].memory; g_bufs[(uint64_t)i[k].buffer].off = i[k].memoryOffset; } }
    return D(d)->BindBufferMemory2(d, n, i);
}
VKAPI_ATTR VkResult VKAPI_CALL L_MapMemory(VkDevice d, VkDeviceMemory m, VkDeviceSize o, VkDeviceSize sz, VkMemoryMapFlags f, void** pp) {
    VkResult r = D(d)->MapMemory(d, m, o, sz, f, pp);
    if (r == VK_SUCCESS) { std::lock_guard<std::mutex> l(g_mutex); g_mapped[(uint64_t)m] = static_cast<char*>(*pp) - o; }
    Ev("MAP mem=%llx off=%llu size=%llu -> %p", (unsigned long long)m, (unsigned long long)o, (unsigned long long)sz, r == VK_SUCCESS ? *pp : nullptr);
    return r;
}
VKAPI_ATTR VkResult VKAPI_CALL L_MapMemory2(VkDevice d, const VkMemoryMapInfo* mi, void** pp) {
    VkResult r = D(d)->MapMemory2(d, mi, pp);
    Ev("MAP2 mem=%llx off=%llu size=%llu -> %p", (unsigned long long)mi->memory, (unsigned long long)mi->offset, (unsigned long long)mi->size, r == VK_SUCCESS ? *pp : nullptr);
    if (r == VK_SUCCESS) { std::lock_guard<std::mutex> l(g_mutex); g_mapped[(uint64_t)mi->memory] = static_cast<char*>(*pp) - mi->offset; }
    return r;
}
VKAPI_ATTR void VKAPI_CALL L_UpdateDescriptorSetWithTemplate(VkDevice d, VkDescriptorSet s, VkDescriptorUpdateTemplate t, const void* data) {
    if (g_template_updates++ == 0) Ev("NOTE: vkUpdateDescriptorSetWithTemplate is used by the engine (not shadowed)");
    D(d)->UpdateDescriptorSetWithTemplate(d, s, t, data);
}
VKAPI_ATTR VkResult VKAPI_CALL L_CreatePipelineLayout(VkDevice d, const VkPipelineLayoutCreateInfo* ci, const VkAllocationCallbacks* a, VkPipelineLayout* l) {
    VkResult r = D(d)->CreatePipelineLayout(d, ci, a, l);
    if (r == VK_SUCCESS) {
        std::string s;
        for (uint32_t i = 0; i < ci->pushConstantRangeCount; ++i)
            s += " pc{stages=" + Hex(ci->pPushConstantRanges[i].stageFlags) + " off=" + std::to_string(ci->pPushConstantRanges[i].offset) + " size=" + std::to_string(ci->pPushConstantRanges[i].size) + "}";
        Ev("PIPELINE_LAYOUT %llx sets=%u%s", (unsigned long long)*l, ci->setLayoutCount, s.c_str());
    }
    return r;
}
VKAPI_ATTR VkResult VKAPI_CALL L_CreateDescriptorSetLayout(VkDevice d, const VkDescriptorSetLayoutCreateInfo* ci, const VkAllocationCallbacks* a, VkDescriptorSetLayout* l) {
    VkResult r = D(d)->CreateDescriptorSetLayout(d, ci, a, l);
    if (r == VK_SUCCESS) {
        std::string s;
        for (uint32_t i = 0; i < ci->bindingCount && i < 40; ++i)
            s += " b" + std::to_string(ci->pBindings[i].binding) + ":t" + std::to_string(int(ci->pBindings[i].descriptorType)) + "x" + std::to_string(ci->pBindings[i].descriptorCount);
        Ev("DSLAYOUT %llx flags=%x bindings=%u%s", (unsigned long long)*l, unsigned(ci->flags), ci->bindingCount, s.c_str());
    }
    return r;
}
VKAPI_ATTR VkResult VKAPI_CALL L_SetDebugUtilsObjectNameEXT(VkDevice d, const VkDebugUtilsObjectNameInfoEXT* ni) {
    if (g_lean && ni && ni->pObjectName && ni->objectType == VK_OBJECT_TYPE_SHADER_MODULE) {
        std::lock_guard<std::mutex> l(g_mutex); g_modname[ni->objectHandle] = ni->pObjectName;
    } else if (!g_lean && ni && ni->pObjectName) {
        uint64_t h = ni->objectHandle;
        std::lock_guard<std::mutex> l(g_mutex);
        if (ni->objectType == VK_OBJECT_TYPE_IMAGE) g_images[h].name = ni->pObjectName;
        else if (ni->objectType == VK_OBJECT_TYPE_IMAGE_VIEW) g_views[h].name = ni->pObjectName;
        else if (ni->objectType == VK_OBJECT_TYPE_PIPELINE) g_pipes[h].name = ni->pObjectName;
        else if (ni->objectType == VK_OBJECT_TYPE_SHADER_MODULE) g_modname[h] = ni->pObjectName;
        g_names[h] = ni->pObjectName;
        if (g_events && ni->objectType != VK_OBJECT_TYPE_COMMAND_BUFFER) { fprintf(g_events, "NAME type=%d %llx \"%s\"\n", int(ni->objectType), (unsigned long long)h, ni->pObjectName); fflush(g_events); }
    }
    return D(d)->SetDebugUtilsObjectNameEXT(d, ni);
}
VKAPI_ATTR void VKAPI_CALL L_CmdBeginLabel(VkCommandBuffer c, const VkDebugUtilsLabelEXT* l) { Fr("cb%llx LABEL_BEGIN %s", (unsigned long long)c, l->pLabelName); D(c)->CmdBeginDebugUtilsLabelEXT(c, l); }
VKAPI_ATTR void VKAPI_CALL L_CmdEndLabel(VkCommandBuffer c) { Fr("cb%llx LABEL_END", (unsigned long long)c); D(c)->CmdEndDebugUtilsLabelEXT(c); }
VKAPI_ATTR void VKAPI_CALL L_CmdInsertLabel(VkCommandBuffer c, const VkDebugUtilsLabelEXT* l) { Fr("cb%llx LABEL_INSERT %s", (unsigned long long)c, l->pLabelName); D(c)->CmdInsertDebugUtilsLabelEXT(c, l); }

void DumpBoundSet(VkCommandBuffer c);
void FlushDraws(VkCommandBuffer c) {
    uint32_t n;
    { std::lock_guard<std::mutex> l(g_mutex); n = g_cbs[(uint64_t)c].draws; g_cbs[(uint64_t)c].draws = 0; }
    if (n) Fr("cb%llx   (%u draw calls)", (unsigned long long)c, n);
}
VKAPI_ATTR VkResult VKAPI_CALL L_BeginCommandBuffer(VkCommandBuffer c, const VkCommandBufferBeginInfo* b) {
    { std::lock_guard<std::mutex> l(g_mutex); g_cbs[(uint64_t)c] = CbInfo(); }
    Fr("cb%llx BEGIN_CB flags=%x", (unsigned long long)c, unsigned(b->flags));
    return D(c)->BeginCommandBuffer(c, b);
}
VKAPI_ATTR void VKAPI_CALL L_CmdBindPipeline(VkCommandBuffer c, VkPipelineBindPoint bp, VkPipeline p) {
    if (bp == VK_PIPELINE_BIND_POINT_COMPUTE) { std::lock_guard<std::mutex> l(g_mutex); g_cbs[(uint64_t)c].compute_pipe = (uint64_t)p; }
    else if (!g_lean) { std::lock_guard<std::mutex> l(g_mutex); g_cbs[(uint64_t)c].gfx_pipe = (uint64_t)p; }
    if (g_capturing && bp == VK_PIPELINE_BIND_POINT_COMPUTE) { FlushDraws(c); Fr("cb%llx BIND_COMPUTE %s", (unsigned long long)c, PipeStr((uint64_t)p).c_str()); }
    D(c)->CmdBindPipeline(c, bp, p);
}
uint32_t CurPipeFlags(VkCommandBuffer c) {
    std::lock_guard<std::mutex> l(g_mutex);
    auto it = g_pipes.find(g_cbs[(uint64_t)c].compute_pipe); return it == g_pipes.end() ? 0 : it->second.flags;
}
bool KnownFsrShader(VkCommandBuffer c, std::initializer_list<uint64_t> hashes) {
    std::lock_guard<std::mutex> l(g_mutex);
    auto p = g_pipes.find(g_cbs[(uint64_t)c].compute_pipe);
    if (p == g_pipes.end()) return false;
    for (uint64_t expected : hashes) if (p->second.module_hash == expected) return true;
    return false;
}
bool BoundImage(VkCommandBuffer c, uint32_t binding, VkImage* img, ImageInfo* info, int* layout) {
    std::lock_guard<std::mutex> l(g_mutex);
    auto ds = g_dsets.find(g_cbs[(uint64_t)c].set0); if (ds == g_dsets.end()) return false;
    auto e = ds->second.find(uint64_t(binding) << 32); if (e == ds->second.end()) return false;
    auto v = g_views.find(e->second.view_or_buf); if (v == g_views.end()) return false;
    auto im = g_images.find(v->second.image); if (im == g_images.end()) return false;
    *img = (VkImage)v->second.image; *info = im->second; *layout = e->second.layout; return true;
}
uint32_t TexelBytes(int fmt) { switch (fmt) { case 97: return 8; case 83: case 100: case 130: case 126: case 37: return 4; default: return 0; } }
bool EnsureDumpBuffer(uint64_t bytes) {
    if (size_t(g_dump_next) < g_dump_allocations.size()) {
        const auto& allocation = g_dump_allocations[g_dump_next];
        g_dump_buf = allocation.buffer; g_dump_ptr = allocation.mapped; g_dump_cap = allocation.capacity;
        return g_dump_cap >= bytes;
    }
    Dev* dev = D(g_device); if (!dev || !g_gipa) return false;
    auto gpdmp = reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(g_gipa(g_instance, "vkGetPhysicalDeviceMemoryProperties"));
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO}; bi.size = bytes; bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    VkBuffer b; if (dev->CreateBuffer(g_device, &bi, nullptr, &b) != VK_SUCCESS) return false;
    VkMemoryRequirements req; dev->GetBufferMemoryRequirements(g_device, b, &req);
    VkPhysicalDeviceMemoryProperties mp; gpdmp(g_pd, &mp); uint32_t ti = UINT32_MAX;
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i) if ((req.memoryTypeBits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) == (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) && !(mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) { ti = i; break; }
    if (ti == UINT32_MAX) return false;
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; ai.allocationSize = req.size; ai.memoryTypeIndex = ti;
    VkDeviceMemory m; if (dev->AllocateMemory(g_device, &ai, nullptr, &m) != VK_SUCCESS) return false;
    dev->BindBufferMemory(g_device, b, m, 0);
    void* p; if (dev->MapMemory(g_device, m, 0, VK_WHOLE_SIZE, 0, &p) != VK_SUCCESS) return false;
    g_dump_buf = b; g_dump_ptr = static_cast<uint8_t*>(p); g_dump_cap = bytes;
    g_dump_allocations.push_back({b, m, g_dump_ptr, bytes});
    Ev("DUMP buffer %llu MiB host memory type %u", (unsigned long long)(bytes >> 20), ti); return true;
}
// Copies one bound image (descriptor layout `layout`) into the dump buffer; transitions to TRANSFER_SRC and back.
bool DumpImage(VkCommandBuffer c, VkImage img, const ImageInfo& ii, int layout, bool is_depth, DumpPart& part, uint64_t slot_base, uint64_t* cursor) {
    Dev* dev = D(c);
    uint32_t tb = TexelBytes(int(ii.fmt)); if (!tb) return false;
    part.off = slot_base + *cursor; part.w = ii.w; part.h = ii.h; part.fmt = int(ii.fmt); part.bytes = uint64_t(ii.w) * ii.h * tb;
    if (part.off > g_dump_cap || part.bytes > g_dump_cap - part.off) { Ev("DUMP: image exceeds per-frame buffer"); return false; }
    *cursor += (part.bytes + 255) & ~255ull;
    VkImageAspectFlags all = is_depth ? (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT) : VK_IMAGE_ASPECT_COLOR_BIT;
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER}; b.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT; b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    b.oldLayout = VkImageLayout(layout); b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL; b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; b.image = img; b.subresourceRange = {all, 0, 1, 0, 1};
    if (layout != VK_IMAGE_LAYOUT_GENERAL) dev->CmdPipelineBarrier(c, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
    else { VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER}; mb.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT; mb.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT; dev->CmdPipelineBarrier(c, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr); }
    VkBufferImageCopy r{}; r.bufferOffset = part.off; r.imageSubresource = {is_depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}; r.imageExtent = {ii.w, ii.h, 1};
    dev->CmdCopyImageToBuffer(c, img, layout == VK_IMAGE_LAYOUT_GENERAL ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g_dump_buf, 1, &r);
    if (layout != VK_IMAGE_LAYOUT_GENERAL) {
        b.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT; b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_MEMORY_READ_BIT; b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL; b.newLayout = VkImageLayout(layout);
        dev->CmdPipelineBarrier(c, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
    } else {
        VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER}; mb.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT; mb.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
        dev->CmdPipelineBarrier(c, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
    }
    return true;
}
void DumpInputs(VkCommandBuffer c) {
    if (!g_dump_active || g_dump_left <= 0 || g_dump_next >= g_dump_total) return;
    VkImage ci, di, mi; ImageInfo cinf, dinf, minf; int cl, dl, ml;
    if (!BoundImage(c, 49, &ci, &cinf, &cl) || !BoundImage(c, 48, &di, &dinf, &dl) || !BoundImage(c, 47, &mi, &minf, &ml)) return;
    uint64_t per = 0; for (auto* i : {&cinf, &dinf, &minf}) per += ((uint64_t(i->w) * i->h * 8) + 255) & ~255ull; per += 3 * uint64_t(2560) * 1440 * 8 + (4u << 20);  // output + exposure reserve + rcas-input regions  // output upper bound + slack
    per = (per + 4095) & ~4095ull;
    if (g_dump_slot_bytes == 0) { g_dump_slot_bytes = per; g_dump.assign(g_dump_total, DumpSlot()); }
    // One allocation per frame: 40 frames exceed RADV's 4 GiB maxBufferSize
    // when packed into the old single buffer.
    if (!EnsureDumpBuffer(g_dump_slot_bytes)) { Ev("DUMP: buffer allocation failed"); g_dump_left = 0; return; }
    DumpSlot& sl = g_dump[g_dump_next]; sl.frame = g_frame; uint64_t cur = 0, base = 0;
    bool ok = DumpImage(c, ci, cinf, cl, false, sl.color, base, &cur) && DumpImage(c, di, dinf, dl, true, sl.depth, base, &cur) && DumpImage(c, mi, minf, ml, false, sl.mv, base, &cur);
    sl.in_ok = ok;
    sl.out.off = base + cur;  // reserve; filled after RCAS
}
void DumpExposure(VkCommandBuffer c) {
    if (!g_dump_active || g_dump_next >= g_dump_total || !g_dump_slot_bytes) return;
    VkImage ei; ImageInfo einf; int el; if (!BoundImage(c, 47, &ei, &einf, &el)) return;
    DumpSlot& sl = g_dump[g_dump_next]; uint64_t cur = sl.out.off + ((uint64_t(2560) * 1440 * 8 + 255) & ~255ull);
    DumpImage(c, ei, einf, el, false, sl.expo, 0, &cur);
}
void DumpRcasInput(VkCommandBuffer c) {
    if (!g_dump_active || g_dump_next >= g_dump_total || !g_dump_slot_bytes) return;
    VkImage ri; ImageInfo rinf; int rl; if (!BoundImage(c, 48, &ri, &rinf, &rl)) return;
    DumpSlot& sl = g_dump[g_dump_next]; uint64_t cur = sl.out.off + 2 * ((uint64_t(2560) * 1440 * 8 + 255) & ~255ull);
    DumpImage(c, ri, rinf, rl, false, sl.rcasin, 0, &cur);
}
// Replaces the RCAS dispatch: copies the accumulate result (RCAS input) straight into RCAS's output image.
void SubstituteRcas(VkCommandBuffer c) {
    VkImage src, dst; ImageInfo si, di; int sl, dl;
    if (!BoundImage(c, 48, &src, &si, &sl) || !BoundImage(c, 31, &dst, &di, &dl) || si.w != di.w || si.h != di.h || si.fmt != di.fmt || dl != VK_IMAGE_LAYOUT_GENERAL) return;
    Dev* dev = D(c);
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER}; b.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT; b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    b.oldLayout = VkImageLayout(sl); b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL; b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; b.image = src; b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    dev->CmdPipelineBarrier(c, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
    VkMemoryBarrier wb{VK_STRUCTURE_TYPE_MEMORY_BARRIER}; wb.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT; wb.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    dev->CmdPipelineBarrier(c, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &wb, 0, nullptr, 0, nullptr);
    VkImageCopy r{}; r.srcSubresource = r.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}; r.extent = {si.w, si.h, 1};
    dev->CmdCopyImage(c, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst, VK_IMAGE_LAYOUT_GENERAL, 1, &r);
    b.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT; b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_MEMORY_READ_BIT; b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL; b.newLayout = VkImageLayout(sl);
    dev->CmdPipelineBarrier(c, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
    VkMemoryBarrier rb{VK_STRUCTURE_TYPE_MEMORY_BARRIER}; rb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; rb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT;
    dev->CmdPipelineBarrier(c, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &rb, 0, nullptr, 0, nullptr);
}
void DumpOutput(VkCommandBuffer c) {
    if (!g_dump_active || g_dump_next >= g_dump_total || !g_dump_slot_bytes) return;
    VkImage oi; ImageInfo oinf; int ol; if (!BoundImage(c, 31, &oi, &oinf, &ol)) return;
    DumpSlot& sl = g_dump[g_dump_next]; if (!sl.in_ok) return;
    uint64_t cur = sl.out.off;
    sl.out_ok = DumpImage(c, oi, oinf, ol, false, sl.out, 0, &cur);
    if (sl.out_ok) { ++g_dump_next; --g_dump_left; }
}
void FinishDump() {
    Dev* dev = D(g_device); dev->DeviceWaitIdle(g_device);
    std::string dir = g_out + "/dump"; mkdir(dir.c_str(), 0755);
    FILE* mf = fopen((dir + "/manifest.txt").c_str(), "w");
    for (int i = 0; i < g_dump_next; ++i) {
        DumpSlot& sl = g_dump[i];
        for (DumpPart* p : {&sl.color, &sl.depth, &sl.mv, &sl.expo, &sl.out, &sl.rcasin}) {
            char fn[256]; snprintf(fn, sizeof fn, "%s/f%02d_%s.raw", dir.c_str(), i, p->name);
            if (FILE* f = fopen(fn, "wb")) { fwrite(g_dump_allocations[i].mapped + p->off, 1, p->bytes, f); fclose(f); }
            fprintf(mf, "slot=%d frame=%llu part=%s w=%u h=%u fmt=%d bytes=%llu\n", i, (unsigned long long)sl.frame, p->name, p->w, p->h, p->fmt, (unsigned long long)p->bytes);
        }
    }
    fclose(mf); Ev("DUMP written: %d frames to %s", g_dump_next, dir.c_str());
    for (auto& allocation : g_dump_allocations) {
        vkUnmapMemory(g_device, allocation.memory);
        vkDestroyBuffer(g_device, allocation.buffer, nullptr);
        vkFreeMemory(g_device, allocation.memory, nullptr);
    }
    g_dump_allocations.clear(); g_dump_buf = VK_NULL_HANDLE; g_dump_ptr = nullptr; g_dump_cap = 0;
    g_dump_active = false; g_dump_left = 0; g_dump_next = 0; g_dump_slot_bytes = 0;
}
// Host-coherent, persistently mapped transfer target filled with the 0xEE "not written" sentinel.
bool MakeHostReadback(VkDeviceSize size, VkBuffer* out, uint8_t** ptr, uint32_t* type) {
    Dev* dev = D(g_device); if (!dev || !g_gipa) return false;
    auto gpdmp = reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(g_gipa(g_instance, "vkGetPhysicalDeviceMemoryProperties"));
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO}; bi.size = size; bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    VkBuffer b; if (dev->CreateBuffer(g_device, &bi, nullptr, &b) != VK_SUCCESS) return false;
    VkMemoryRequirements req; dev->GetBufferMemoryRequirements(g_device, b, &req);
    VkPhysicalDeviceMemoryProperties mp; gpdmp(g_pd, &mp);
    uint32_t ti = UINT32_MAX;
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i) if ((req.memoryTypeBits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) == (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) { ti = i; break; }
    if (ti == UINT32_MAX) return false;
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; ai.allocationSize = req.size; ai.memoryTypeIndex = ti;
    VkDeviceMemory m; if (dev->AllocateMemory(g_device, &ai, nullptr, &m) != VK_SUCCESS) return false;
    dev->BindBufferMemory(g_device, b, m, 0);
    void* p; if (dev->MapMemory(g_device, m, 0, VK_WHOLE_SIZE, 0, &p) != VK_SUCCESS) return false;
    memset(p, 0xEE, size);
    *ptr = static_cast<uint8_t*>(p); *out = b; *type = ti;
    return true;
}
bool EnsureReadback() {
    if (g_rb) return true;
    uint32_t ti; if (!MakeHostReadback(kRbSlots * kRbBytes, &g_rb, &g_rb_ptr, &ti)) return false;
    if (!g_lean) g_rb_log = fopen((g_out + "/cb_readback.log").c_str(), "w");
    Ev("READBACK buffer ready (host-visible memory type %u)", ti);
    return true;
}
void InjectCbReadback(VkCommandBuffer c) {
    uint64_t set; { std::lock_guard<std::mutex> l(g_mutex); set = g_cbs[(uint64_t)c].set0; }
    Desc d{}; { std::lock_guard<std::mutex> l(g_mutex); auto it = g_dsets.find(set); if (it == g_dsets.end()) return; auto e = it->second.find(uint64_t(16) << 32); if (e == it->second.end()) return; d = e->second; }
    if (!d.view_or_buf || !EnsureReadback()) return;
    Dev* dev = D(c);
    int slot = int(g_frame % kRbSlots);
    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER}; mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT; mb.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    dev->CmdPipelineBarrier(c, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
    VkBufferCopy r{d.off, uint64_t(slot) * kRbBytes, d.range < kRbBytes ? d.range : kRbBytes};
    dev->CmdCopyBuffer(c, (VkBuffer)d.view_or_buf, g_rb, 1, &r);
    g_rb_frame[slot] = g_frame;
}
void DrainReadback() {
    if (!g_rb_ptr) return;
    for (int s = 0; s < kRbSlots; ++s) {
        const uint32_t* w = reinterpret_cast<const uint32_t*>(g_rb_ptr + s * kRbBytes);
        if (w[0] == 0xEEEEEEEEu && w[1] == 0xEEEEEEEEu) continue;
        auto f = [&](int i) { float v; memcpy(&v, &w[i], 4); return v; };
        fprintf(g_rb_log, "frame=%llu render=%dx%d prevRender=%dx%d upscale=%dx%d prevUpscale=%dx%d maxRender=%dx%d maxUpscale=%dx%d d2v=[%g %g %g %g] jitter=[%.4f %.4f] prevJitter=[%.4f %.4f] mvScale=[%g %g] downscale=[%g %g] mvJitCancel=[%g %g] tanHalfFov=%g jitterSeqLen=%g dt=%g preExpDelta=%g vs2m=%g frameIdx=%g velFactor=%g reactScale=%g shadingScale=%g accumAdd=%g minDisocc=%g\n",
                (unsigned long long)g_rb_frame[s], int(w[0]), int(w[1]), int(w[2]), int(w[3]), int(w[4]), int(w[5]), int(w[6]), int(w[7]), int(w[8]), int(w[9]), int(w[10]), int(w[11]),
                f(12), f(13), f(14), f(15), f(16), f(17), f(18), f(19), f(20), f(21), f(22), f(23), f(24), f(25), f(26), f(27), f(28), f(29), f(30), f(31), f(32), f(33), f(34), f(35));
        memset(g_rb_ptr + s * kRbBytes, 0xEE, kRbBytes);
    }
    fflush(g_rb_log);
}

// ================= FSR 4 PoC: replace RCAS with the Vulkan FSR 4.1.1 backend =================
struct Fsr4In { VkImage ci = VK_NULL_HANDLE, di = VK_NULL_HANDLE, mi = VK_NULL_HANDLE; VkImageView cv = VK_NULL_HANDLE, dv = VK_NULL_HANDLE, mv = VK_NULL_HANDLE; ImageInfo cinf{}, dinf{}, minf{}; int cl = 0, dl = 0, ml = 0; bool ok = false; };
struct Fsr4State {
    std::unique_ptr<Fsr411::Upscaler> up; bool failed = false;
    uint64_t counter = 0; int64_t offset = 0; bool have_offset = false; bool need_reset = true;
    uint64_t last_used = 0; uint32_t seq_len = 0, rw = 0, rh = 0, ow = 0, oh = 0;
    std::vector<std::array<float, 2>> table; std::vector<char> have;
    uint64_t used = 0, failopen = 0; std::string why;
    uint64_t rb_counter[kRbSlots] = {};
    struct Prediction { uint64_t counter = 0; float x = 0, y = 0; bool valid = false; } predictions[kRbSlots];
    uint64_t jitter_checked = 0, jitter_mismatch = 0;
    uint32_t backend_ow = 0, backend_oh = 0; bool backend_ultra = false;
} g_f4;
Fsr4In g_in;
VkBuffer g_weight_rb = VK_NULL_HANDLE;
uint8_t* g_weight_ptr = nullptr;
std::vector<uint8_t> g_weight_expected;
uint64_t g_weight_record_counter = 0;
bool g_weight_checked = false;

// FSR 4 sharpening. Default: follow the game's sharpness slider, read back from RCAS's cbRCAS
// (binding 17). FSR4STUDY_SHARPNESS=x fixes it, FSR4STUDY_NO_SHARPEN turns it off.
enum class SharpMode { Follow, Fixed, Off };
SharpMode g_sharp_mode = SharpMode::Follow;
float g_f4_sharp = -1.0f;  // < 0: no RCAS
VkBuffer g_rcas_rb = VK_NULL_HANDLE; uint8_t* g_rcas_ptr = nullptr; uint64_t g_rcas_counter[kRbSlots] = {};
constexpr int kRcasBytes = 16;
void InitSharpness() {
    static bool done = false; if (done) return; done = true;
    if (getenv("FSR4STUDY_NO_SHARPEN")) { g_sharp_mode = SharpMode::Off; g_f4_sharp = -1.0f; Ev("FSR4: sharpening off"); }
    else if (const char* sv = getenv("FSR4STUDY_SHARPNESS")) { g_sharp_mode = SharpMode::Fixed; g_f4_sharp = std::clamp(float(atof(sv)), 0.0f, 1.0f); Ev("FSR4: sharpening fixed at %.3f", g_f4_sharp); }
    else Ev("FSR4: sharpening follows the game's RCAS setting");
}
// RCAS reads rcasConfig.x = exp2(-(2 - 2 * sharpness)) (FidelityFX FsrRcasCon); FSR 4's RCAS takes the same 0..1 value.
void F4InjectRcasReadback(VkCommandBuffer c) {
    if (g_sharp_mode != SharpMode::Follow) return;
    uint64_t set; { std::lock_guard<std::mutex> l(g_mutex); set = g_cbs[(uint64_t)c].set0; }
    Desc d{}; { std::lock_guard<std::mutex> l(g_mutex); auto it = g_dsets.find(set); if (it == g_dsets.end()) return; auto e = it->second.find(uint64_t(17) << 32); if (e == it->second.end()) return; d = e->second; }
    if (!d.view_or_buf) return;
    if (!g_rcas_rb) { uint32_t ti; if (!MakeHostReadback(kRbSlots * kRcasBytes, &g_rcas_rb, &g_rcas_ptr, &ti)) return; }
    Dev* dev = D(c); int slot = int(g_f4.counter % kRbSlots);
    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER}; mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_HOST_WRITE_BIT; mb.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    dev->CmdPipelineBarrier(c, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT | VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
    VkBufferCopy r{d.off, uint64_t(slot) * kRcasBytes, kRcasBytes};
    dev->CmdCopyBuffer(c, (VkBuffer)d.view_or_buf, g_rcas_rb, 1, &r);
    VkMemoryBarrier back{VK_STRUCTURE_TYPE_MEMORY_BARRIER}; back.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; back.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    dev->CmdPipelineBarrier(c, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &back, 0, nullptr, 0, nullptr);
    g_rcas_counter[slot] = g_f4.counter;
}
// Takes the newest completed cbRCAS copy; until one lands FSR 4 keeps the last value (no RCAS at start).
void F4ProcessRcasReadback() {
    if (g_sharp_mode != SharpMode::Follow || !g_rcas_ptr) return;
    uint64_t best = 0; float best_v = 0; bool found = false;
    for (int s = 0; s < kRbSlots; ++s) {
        uint32_t w[kRcasBytes / 4]; memcpy(w, g_rcas_ptr + s * kRcasBytes, kRcasBytes);
        if (w[0] == 0xEEEEEEEEu && w[1] == 0xEEEEEEEEu) continue;
        memset(g_rcas_ptr + s * kRcasBytes, 0xEE, kRcasBytes);
        float v; memcpy(&v, &w[0], 4);
        if (!found || g_rcas_counter[s] > best) { best = g_rcas_counter[s]; best_v = v; found = true; }
    }
    if (!found) return;
    if (!std::isfinite(best_v) || best_v <= 0.0f || best_v > 1.0001f) {
        static uint32_t bad = 0; if (bad++ < 8) Ev("FSR4: ignored invalid cbRCAS value %g (counter %llu)", best_v, (unsigned long long)best);
        return;
    }
    const float s = std::clamp((std::log2(best_v) + 2.0f) * 0.5f, 0.0f, 1.0f);
    if (g_f4_sharp < 0.0f || std::abs(s - g_f4_sharp) > 1e-4f) Ev("FSR4: sharpness now %.3f from the game's RCAS (rcasConfig.x=%g, counter %llu)", s, best_v, (unsigned long long)best);
    g_f4_sharp = s;
}

// When FSR 4 replaced the previous frame, do not dispatch the FSR 3 chain (clears .. accumulate)
// that FSR 4 does not read; FSR4STUDY_KEEP_FSR3=1 keeps it. The decision is made at the chain's
// first dispatch, before RCAS-time checks run, so RCAS must cope with a frame without an FSR 3 result.
const bool g_skip_fsr3 = getenv("FSR4STUDY_KEEP_FSR3") == nullptr;
constexpr int kHandbackFrames = 32;  // FSR 3 re-accumulates this long before a planned switch to it
struct SkipState {
    bool chain_open = false, skip = false, last_soft = false;
    uint64_t chain_frame = 0, chains = 0, dispatches = 0, soft = 0, hard = 0;
    int handback = 0;
} g_skip;
// Runtime control files in g_out. Diag mode stats them where they are used; lean mode reads bits that
// the present hook refreshes every 8th frame (one stat per file instead of several per frame).
enum OutFile { kFsr4Disable, kSkipDisable, kRejectTrigger, kOutFileCount };
const char* const kOutFileNames[kOutFileCount] = {"fsr4.disable", "fsr3skip.disable", "reject.trigger"};
std::atomic<uint32_t> g_out_files{0};
bool StatOut(const char* name) { struct stat st; return stat((g_out + "/" + name).c_str(), &st) == 0; }
bool OutFileExists(OutFile f) { return g_lean ? (g_out_files.load(std::memory_order_relaxed) >> f) & 1u : StatOut(kOutFileNames[f]); }
void PollOutFiles() {
    uint32_t bits = 0;
    for (int f = 0; f < kOutFileCount; ++f) if ((f != kRejectTrigger || getenv("FSR4STUDY_TEST_REJECT")) && StatOut(kOutFileNames[f])) bits |= 1u << f;
    g_out_files.store(bits, std::memory_order_relaxed);
}
bool F4ShouldSkipChain() {
    // Dumps compare against FSR 3's accumulate image, which a skipped chain leaves stale.
    static const bool keep_during_dump = getenv("FSR4STUDY_SKIP_DURING_DUMP") != nullptr;
    if (!g_skip_fsr3 || g_f4.failed || !g_f4.up || (g_dump_active && !keep_during_dump) || g_skip.last_soft) return false;
    if (!g_f4.used || g_f4.last_used != g_f4.counter) return false;  // previous frame was not FSR 4
    if (OutFileExists(kFsr4Disable)) return false;
    if (OutFileExists(kSkipDisable)) return false;  // runtime A/B
    static const bool test_reject = getenv("FSR4STUDY_TEST_REJECT") != nullptr;
    if (test_reject && OutFileExists(kRejectTrigger)) return false;
    return true;
}
// FSR4STUDY_TIME_CHAIN=1: GPU time of the whole upscale block (first upscale dispatch .. RCAS or its
// FSR 4 replacement), read back without waiting a few frames later, median per mode every 300 frames.
struct ChainTimer {
    static constexpr uint32_t kSlots = 8;
    VkQueryPool pool = VK_NULL_HANDLE; float period_ns = 1; bool open = false; uint64_t open_frame = 0; uint32_t next = 0;
    int pending[kSlots] = {-1, -1, -1, -1, -1, -1, -1, -1};  // mode per in-flight slot
    std::vector<double> ms[3];
} g_timer;
const char* const kChainMode[3] = {"FSR3", "FSR4", "FSR4+skipFSR3"};
void ChainTimerCollect(Dev* dev) {
    auto get = reinterpret_cast<PFN_vkGetQueryPoolResults>(dev->gdpa(g_device, "vkGetQueryPoolResults"));
    for (uint32_t s = 0; s < ChainTimer::kSlots; ++s) {
        if (g_timer.pending[s] < 0) continue;
        uint64_t ts[2];
        if (get(g_device, g_timer.pool, 2 * s, 2, sizeof ts, ts, 8, VK_QUERY_RESULT_64_BIT) != VK_SUCCESS) continue;
        auto& v = g_timer.ms[g_timer.pending[s]]; v.push_back(double(ts[1] - ts[0]) * g_timer.period_ns * 1e-6);
        if (v.size() == 300) {
            std::sort(v.begin(), v.end());
            Ev("CHAIN GPU time mode=%s median=%.3f ms p10=%.3f p90=%.3f (300 frames)", kChainMode[g_timer.pending[s]], v[150], v[30], v[270]);
            v.clear();
        }
        g_timer.pending[s] = -1;
    }
}
void ChainTimerBegin(VkCommandBuffer c) {
    static const bool on = getenv("FSR4STUDY_TIME_CHAIN") != nullptr;
    if (!on || (g_timer.open && g_timer.open_frame == g_frame)) return;
    Dev* dev = D(c);
    if (!g_timer.pool) {
        VkQueryPoolCreateInfo qi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO}; qi.queryType = VK_QUERY_TYPE_TIMESTAMP; qi.queryCount = 2 * ChainTimer::kSlots;
        if (reinterpret_cast<PFN_vkCreateQueryPool>(dev->gdpa(g_device, "vkCreateQueryPool"))(g_device, &qi, nullptr, &g_timer.pool) != VK_SUCCESS) return;
        VkPhysicalDeviceProperties pp; reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(g_gipa(g_instance, "vkGetPhysicalDeviceProperties"))(g_pd, &pp);
        g_timer.period_ns = pp.limits.timestampPeriod;
    }
    ChainTimerCollect(dev);
    const uint32_t s = g_timer.next;
    if (g_timer.pending[s] >= 0) return;  // slot still in flight: skip this frame
    reinterpret_cast<PFN_vkCmdResetQueryPool>(dev->gdpa(g_device, "vkCmdResetQueryPool"))(c, g_timer.pool, 2 * s, 2);
    reinterpret_cast<PFN_vkCmdWriteTimestamp>(dev->gdpa(g_device, "vkCmdWriteTimestamp"))(c, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, g_timer.pool, 2 * s);
    g_timer.open = true; g_timer.open_frame = g_frame;
}
void ChainTimerEnd(VkCommandBuffer c, bool fsr4) {
    if (!g_timer.open) return;
    g_timer.open = false;
    const uint32_t s = g_timer.next; g_timer.next = (s + 1) % ChainTimer::kSlots;
    reinterpret_cast<PFN_vkCmdWriteTimestamp>(D(c)->gdpa(g_device, "vkCmdWriteTimestamp"))(c, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, g_timer.pool, 2 * s + 1);
    g_timer.pending[s] = fsr4 ? (g_skip.skip ? 2 : 1) : 0;
}

bool BoundView(VkCommandBuffer c, uint32_t binding, VkImageView* view, VkImage* img, ImageInfo* info, int* layout) {
    std::lock_guard<std::mutex> l(g_mutex);
    auto ds = g_dsets.find(g_cbs[(uint64_t)c].set0); if (ds == g_dsets.end()) return false;
    auto e = ds->second.find(uint64_t(binding) << 32); if (e == ds->second.end()) return false;
    auto v = g_views.find(e->second.view_or_buf); if (v == g_views.end()) return false;
    auto im = g_images.find(v->second.image); if (im == g_images.end()) return false;
    *view = (VkImageView)e->second.view_or_buf; *img = (VkImage)v->second.image; *info = im->second; *layout = e->second.layout; return true;
}
void F4CaptureInputs(VkCommandBuffer c) {
    Fsr4In in;
    in.ok = BoundView(c, 49, &in.cv, &in.ci, &in.cinf, &in.cl) && BoundView(c, 48, &in.dv, &in.di, &in.dinf, &in.dl) && BoundView(c, 47, &in.mv, &in.mi, &in.minf, &in.ml);
    g_in = in;
}
// Consumes the late CB readback: learns the periodic jitter table and the frameIdx <-> counter offset.
void F4ProcessReadback() {
    if (!g_rb_ptr) return;
    struct Snapshot { int slot; uint64_t counter; std::array<uint32_t, kRbBytes / sizeof(uint32_t)> words; };
    std::vector<Snapshot> pending;
    for (int s = 0; s < kRbSlots; ++s) {
        // Snapshot before retiring the mapped slot: f() must never read the
        // 0xEE sentinel written below (it used to poison every learned jitter).
        Snapshot snapshot{s, g_f4.rb_counter[s], {}};
        memcpy(snapshot.words.data(), g_rb_ptr + s * kRbBytes, kRbBytes);
        const uint32_t* w = snapshot.words.data();
        if (w[0] == 0xEEEEEEEEu && w[1] == 0xEEEEEEEEu) continue;
        memset(g_rb_ptr + s * kRbBytes, 0xEE, kRbBytes);
        pending.push_back(snapshot);
    }
    // Physical slot order wraps at 16; reset/phase updates must be chronological.
    std::sort(pending.begin(), pending.end(), [](const auto& a, const auto& b) { return a.counter < b.counter; });
    for (auto& snapshot : pending) {
        const int s = snapshot.slot;
        const uint32_t* w = snapshot.words.data();
        auto f = [&](int i) { float v; memcpy(&v, &w[i], 4); return v; };
        if (!std::isfinite(f(27)) || f(27) < 1 || f(27) > 64 || !std::isfinite(f(31)) || f(31) < 0 || f(31) > 16777216.0f) continue;
        uint32_t rw = w[0], rh = w[1], ow = w[4], oh = w[5]; uint32_t seq = uint32_t(f(27) + 0.5f); int64_t idx = int64_t(f(31) + 0.5f);
        uint64_t cnt = snapshot.counter;
        if (!seq || seq > 64 || !rw || !rh) continue;
        if (seq != g_f4.seq_len || rw != g_f4.rw || rh != g_f4.rh || ow != g_f4.ow || oh != g_f4.oh) {
            g_f4.seq_len = seq; g_f4.rw = rw; g_f4.rh = rh; g_f4.ow = ow; g_f4.oh = oh;
            g_f4.table.assign(seq, {0, 0}); g_f4.have.assign(seq, 0); g_f4.need_reset = true; g_f4.have_offset = false;
            Ev("FSR4: parameters changed -> render %ux%u output %ux%u seqLen %u; relearning jitter table", rw, rh, ow, oh, seq);
        }
        int64_t off = idx - int64_t(cnt);
        if (!g_f4.have_offset || off != g_f4.offset) {
            g_f4.offset = off; g_f4.have_offset = true; g_f4.need_reset = true;
            // A same-size reset can restart Halton at a different phase. Never
            // reuse the preceding phase table while learning the new sequence.
            std::fill(g_f4.have.begin(), g_f4.have.end(), 0);
            Ev("FSR4: frameIdx offset now %lld (counter %llu, frameIdx %lld); invalidated jitter table", (long long)off, (unsigned long long)cnt, (long long)idx);
        }
        const float jx = f(16), jy = f(17);
        if (idx < 0 || !std::isfinite(jx) || !std::isfinite(jy) || std::abs(jx) > 1.0f || std::abs(jy) > 1.0f) {
            Ev("FSR4: rejected invalid jitter readback counter=%llu jitter=[%g %g]", (unsigned long long)cnt, jx, jy);
            continue;
        }
        auto& prediction = g_f4.predictions[s];
        if (prediction.valid && prediction.counter == cnt) {
            const bool mismatch = prediction.x != jx || prediction.y != jy;
            ++g_f4.jitter_checked; g_f4.jitter_mismatch += mismatch;
            if (mismatch) Ev("FSR4: jitter mismatch counter=%llu predicted=[%.9g %.9g] actual=[%.9g %.9g]", (unsigned long long)cnt, prediction.x, prediction.y, jx, jy);
            if (g_f4.jitter_checked % 600 == 0) Ev("FSR4: jitter verification checked=%llu mismatches=%llu", (unsigned long long)g_f4.jitter_checked, (unsigned long long)g_f4.jitter_mismatch);
            prediction.valid = false;
        }
        uint32_t ph = uint32_t(idx % int64_t(seq)); g_f4.table[ph] = {jx, jy}; g_f4.have[ph] = 1;
    }
}
// FSR 4.1.2 has two INT8 providers of the same model. AMD's DLL picks the Navi2 one for GPU family NV with a Navi 2x
// revision (RX 6000) and the generic one otherwise; the layer does the same by PCI device ID (Navi 21-23: 0x73a0-0x73ff,
// Navi 24: 0x7420-0x743f) and loads <assets>/navi2 when it exists. FSR4STUDY_VARIANT=generic|navi2 overrides.
std::string F4AssetDir(const std::string& base) {
    VkPhysicalDeviceProperties pp; reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(g_gipa(g_instance, "vkGetPhysicalDeviceProperties"))(g_pd, &pp);
    const uint32_t id = pp.deviceID;
    const bool navi2x = pp.vendorID == 0x1002 && ((id >= 0x73a0 && id <= 0x73ff) || (id >= 0x7420 && id <= 0x743f));
    const char* v = getenv("FSR4STUDY_VARIANT");
    const bool want = v && *v ? !strcmp(v, "navi2") : navi2x;
    const std::string dir = base + "/navi2";
    struct stat st{};
    const bool have = stat(dir.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
    Ev("FSR4: GPU %04x:%04x navi2x=%d variant=%s -> INT8 %s", pp.vendorID, id, int(navi2x), v && *v ? v : "auto", want && have ? "navi2" : "generic");
    return want && have ? dir : base;
}
// Returns true when FSR 4 produced the output (the original RCAS dispatch is then skipped).
bool F4TryReplaceRcas(VkCommandBuffer c) {
    // A skipped chain left no FSR 3 result: failing open would show a stale image.
    const bool skipped = g_skip.skip;
    auto fail = [&](const char* why) {
        ++g_f4.failopen; if (g_f4.why != why) { g_f4.why = why; Ev("FSR4: fail-open (%s)", why); }
        if (skipped) { ++g_skip.hard; Ev("FSR4: FSR 3 chain was skipped; stale output this frame (%s)", why); }
        return false;
    };
    // Soft reasons come from the late CB readback (resets, size changes). On a skipped frame,
    // run FSR 4 with a history reset instead; the next chain runs FSR 3 normally.
    const char* soft = nullptr;
    auto soft_fail = [&](const char* why) { if (!skipped) return fail(why); if (!soft) soft = why; return true; };
    if (g_f4.failed) return false;
    // Explicit test mode: corrupt only local validation metadata, before any
    // transitions/commands. Production launches never read this trigger.
    char reject[32] = {};
    if (getenv("FSR4STUDY_TEST_REJECT")) if (FILE* f = fopen((g_out + "/reject.trigger").c_str(), "r")) { (void)fscanf(f, "%31s", reject); fclose(f); }
    if (!strcmp(reject, "shader") || !KnownFsrShader(c, {0xdfe007620205b3b2ull})) return fail("RCAS shader fingerprint");
    if (!g_fsr4_features) return fail("device features not enabled");
    F4ProcessReadback(); F4ProcessRcasReadback();
    if (OutFileExists(kFsr4Disable)) {
        // FSR 3 history is stale after skipping: keep FSR 4 on screen while it re-accumulates.
        if (!g_skip_fsr3 || g_skip.handback >= kHandbackFrames) return fail("file toggle disabled");
        ++g_skip.handback;
    } else g_skip.handback = 0;
    if (!g_in.ok) return fail("inputs not captured");
    VkImageView ov; VkImage oi; ImageInfo oinf; int ol;
    if (!strcmp(reject, "binding") || !BoundView(c, 31, &ov, &oi, &oinf, &ol) || ol != VK_IMAGE_LAYOUT_GENERAL) return fail("output binding unexpected");
    if (!strcmp(reject, "size")) oinf.w ^= 1;
    if (!strcmp(reject, "format")) oinf.fmt = VK_FORMAT_UNDEFINED;
    if (!g_f4.have_offset || !g_f4.seq_len) { if (!soft_fail("no CB readback yet")) return false; }
    else if (g_in.cinf.w != g_f4.rw || g_in.cinf.h != g_f4.rh || oinf.w != g_f4.ow || oinf.h != g_f4.oh) { if (!soft_fail("size mismatch with CB")) return false; }
    if (g_in.cinf.fmt != 97 && g_in.cinf.fmt != VK_FORMAT_R16G16B16A16_SFLOAT) return fail("color format");
    if (oinf.fmt != VK_FORMAT_R16G16B16A16_SFLOAT || !(oinf.usage & VK_IMAGE_USAGE_STORAGE_BIT)) return fail("output format/usage");
    if (g_in.dinf.fmt != VK_FORMAT_D32_SFLOAT_S8_UINT || g_in.minf.fmt != VK_FORMAT_R16G16_SFLOAT) return fail("depth/motion format");
    if (g_in.dinf.w != g_in.cinf.w || g_in.dinf.h != g_in.cinf.h || g_in.minf.w != g_in.cinf.w || g_in.minf.h != g_in.cinf.h) return fail("depth/motion size");
    if (!(g_in.cinf.usage & VK_IMAGE_USAGE_SAMPLED_BIT) || !(g_in.dinf.usage & VK_IMAGE_USAGE_SAMPLED_BIT) || !(g_in.minf.usage & VK_IMAGE_USAGE_SAMPLED_BIT)) return fail("input usage");
    int64_t idx = int64_t(g_f4.counter) + g_f4.offset; uint32_t ph = 0;
    if (!soft) {
        ph = uint32_t(((idx % int64_t(g_f4.seq_len)) + int64_t(g_f4.seq_len)) % int64_t(g_f4.seq_len));
        if (!g_f4.have[ph] && !soft_fail("jitter phase not learned yet")) return false;
    }
    static const uint64_t start_after = getenv("FSR4STUDY_START_AFTER") ? strtoull(getenv("FSR4STUDY_START_AFTER"), nullptr, 10) : 0;
    if (g_f4.counter < start_after) return fail("warm-up delay");
    Dev* dev = D(c);
    FinishUniformProbe();
    if (g_weight_ptr && !g_weight_checked && g_f4.counter > g_weight_record_counter + 16) {
        dev->DeviceWaitIdle(g_device);
        size_t different = 0, nonzero = 0;
        for (size_t i = 0; i < g_weight_expected.size(); ++i) { different += g_weight_ptr[i] != g_weight_expected[i]; nonzero += g_weight_ptr[i] != 0; }
        Ev("FSR4: weight readback bytes=%zu different=%zu nonzero=%zu expectedHash=%llx actualHash=%llx", g_weight_expected.size(), different, nonzero, (unsigned long long)Fnv(g_weight_expected.data(), g_weight_expected.size()), (unsigned long long)Fnv(g_weight_ptr, g_weight_expected.size()));
        g_weight_checked = true;
    }
    if (!g_f4.up) {
        const char* a = getenv("FSR4STUDY_ASSETS");
        g_f4.up = std::make_unique<Fsr411::Upscaler>(g_pd, g_device, F4AssetDir(a ? a : "assets"), g_fsr4_valve, g_fsr4_wave);
        Ev("FSR4: backend constructed (valve dot=%d, per-pass wave size=%d, lean=%d)", int(g_fsr4_valve), int(g_fsr4_wave), int(g_lean));
    }
    CbInfo game_state;
    PipeInfo game_pipe;
    { std::lock_guard<std::mutex> lock(g_mutex); game_state = g_cbs[(uint64_t)c]; game_pipe = g_pipes[game_state.compute_pipe]; }
    // layouts: the backend wants GENERAL on all images
    auto barrier = [&](VkImage im, VkImageAspectFlags asp, int from, int to, VkPipelineStageFlags ss, VkPipelineStageFlags ds, VkAccessFlags sa, VkAccessFlags da) {
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER}; b.srcAccessMask = sa; b.dstAccessMask = da; b.oldLayout = VkImageLayout(from); b.newLayout = VkImageLayout(to);
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; b.image = im; b.subresourceRange = {asp, 0, 1, 0, 1};
        dev->CmdPipelineBarrier(c, ss, ds, 0, 0, nullptr, 0, nullptr, 1, &b);
    };
    const auto rd = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    static VkImage own_o = VK_NULL_HANDLE; static VkImageView own_ov = VK_NULL_HANDLE;
    static uint32_t own_ow = 0, own_oh = 0;
    if (getenv("FSR4STUDY_OWN_OUTPUT")) {
        if (!own_o || own_ow != oinf.w || own_oh != oinf.h) {
            // Diagnostic allocations live until process exit; no in-flight object is destroyed.
            VkImageCreateInfo ic{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO}; ic.imageType = VK_IMAGE_TYPE_2D; ic.format = VK_FORMAT_R16G16B16A16_SFLOAT; ic.extent = {oinf.w, oinf.h, 1}; ic.mipLevels = ic.arrayLayers = 1; ic.samples = VK_SAMPLE_COUNT_1_BIT; ic.tiling = VK_IMAGE_TILING_OPTIMAL;
            ic.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
            if (dev->CreateImage(g_device, &ic, nullptr, &own_o) != VK_SUCCESS) { g_f4.failed = true; return fail("owned output image allocation"); }
            VkMemoryRequirements req; dev->GetImageMemoryRequirements(g_device, own_o, &req);
            VkPhysicalDeviceMemoryProperties mp; reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(g_gipa(g_instance, "vkGetPhysicalDeviceMemoryProperties"))(g_pd, &mp);
            uint32_t ti = UINT32_MAX; for (uint32_t i = 0; i < mp.memoryTypeCount; ++i) if ((req.memoryTypeBits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) { ti = i; break; }
            VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; ai.allocationSize = req.size; ai.memoryTypeIndex = ti; VkDeviceMemory mem;
            if (ti == UINT32_MAX || dev->AllocateMemory(g_device, &ai, nullptr, &mem) != VK_SUCCESS || dev->BindImageMemory(g_device, own_o, mem, 0) != VK_SUCCESS) { g_f4.failed = true; return fail("owned output memory allocation"); }
            VkImageViewCreateInfo vc{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO}; vc.image = own_o; vc.viewType = VK_IMAGE_VIEW_TYPE_2D; vc.format = ic.format; vc.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            if (dev->CreateImageView(g_device, &vc, nullptr, &own_ov) != VK_SUCCESS) { g_f4.failed = true; return fail("owned output view allocation"); }
            barrier(own_o, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, rd);
            own_ow = oinf.w; own_oh = oinf.h;
            Ev("FSR4: using layer-owned output %ux%u", own_ow, own_oh);
        }
        barrier(own_o, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, rd);
    }
    const VkImageAspectFlags depth_all = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER}; mb.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT; mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    dev->CmdPipelineBarrier(c, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
    barrier(g_in.ci, VK_IMAGE_ASPECT_COLOR_BIT, g_in.cl, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_MEMORY_WRITE_BIT, rd);
    barrier(g_in.di, depth_all, g_in.dl, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_MEMORY_WRITE_BIT, rd);
    barrier(g_in.mi, VK_IMAGE_ASPECT_COLOR_BIT, g_in.ml, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_MEMORY_WRITE_BIT, rd);

    if (getenv("FSR4STUDY_CLEAR_OUT")) {  // debug: prove whether the backend writes the output at all
        VkClearColorValue magenta{{1.0f, 0.0f, 1.0f, 1.0f}}; VkImageSubresourceRange rg{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkMemoryBarrier cb{VK_STRUCTURE_TYPE_MEMORY_BARRIER}; cb.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_MEMORY_READ_BIT; cb.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        dev->CmdPipelineBarrier(c, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &cb, 0, nullptr, 0, nullptr);
        dev->CmdClearColorImage(c, oi, VK_IMAGE_LAYOUT_GENERAL, &magenta, 1, &rg);
        cb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; cb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        dev->CmdPipelineBarrier(c, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &cb, 0, nullptr, 0, nullptr);
    }
    static bool logged_views = false;
    if (!logged_views) {
        logged_views = true;
        for (auto* p : {&g_in.cv, &g_in.dv, &g_in.mv, &ov}) {
            ViewInfo vi{}; { std::lock_guard<std::mutex> l(g_mutex); auto it = g_views.find((uint64_t)*p); if (it != g_views.end()) vi = it->second; }
            Ev("FSR4: view %llx: type=%d fmt=%d aspect=%x mips=%u+%u layers=%u+%u swizzle=%08x", (unsigned long long)*p, vi.type, int(vi.fmt), vi.aspect, vi.base_mip, vi.mips, vi.base_layer, vi.layers, vi.swz);
        }
    }
    static VkImage own_c = VK_NULL_HANDLE; static VkImageView own_cv = VK_NULL_HANDLE;
    static const bool use_own_color = getenv("FSR4STUDY_OWN_COLOR") != nullptr;
    VkImage use_ci = g_in.ci; VkImageView use_cv = g_in.cv;
    if (use_own_color) {
        if (!own_c) {
            VkImageCreateInfo ic{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO}; ic.imageType = VK_IMAGE_TYPE_2D; ic.format = VK_FORMAT_R16G16B16A16_SFLOAT; ic.extent = {g_in.cinf.w, g_in.cinf.h, 1}; ic.mipLevels = 1; ic.arrayLayers = 1; ic.samples = VK_SAMPLE_COUNT_1_BIT; ic.tiling = VK_IMAGE_TILING_OPTIMAL; ic.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
            dev->CreateImage(g_device, &ic, nullptr, &own_c);
            VkMemoryRequirements req; dev->GetImageMemoryRequirements(g_device, own_c, &req);
            auto gpdmp = reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(g_gipa(g_instance, "vkGetPhysicalDeviceMemoryProperties")); VkPhysicalDeviceMemoryProperties mp; gpdmp(g_pd, &mp);
            uint32_t ti = 0; for (uint32_t i = 0; i < mp.memoryTypeCount; ++i) if ((req.memoryTypeBits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) { ti = i; break; }
            VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; ai.allocationSize = req.size; ai.memoryTypeIndex = ti; VkDeviceMemory m; dev->AllocateMemory(g_device, &ai, nullptr, &m); dev->BindImageMemory(g_device, own_c, m, 0);
            VkImageViewCreateInfo vc{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO}; vc.image = own_c; vc.viewType = VK_IMAGE_VIEW_TYPE_2D; vc.format = VK_FORMAT_R16G16B16A16_SFLOAT; vc.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            reinterpret_cast<PFN_vkCreateImageView>(dev->gdpa(g_device, "vkCreateImageView"))(g_device, &vc, nullptr, &own_cv);
            barrier(own_c, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
            Ev("FSR4: using layer-owned color copy %ux%u", g_in.cinf.w, g_in.cinf.h);
        }
        barrier(g_in.ci, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
        VkImageCopy rg{}; rg.srcSubresource = rg.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}; rg.extent = {g_in.cinf.w, g_in.cinf.h, 1};
        dev->CmdCopyImage(c, g_in.ci, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, own_c, VK_IMAGE_LAYOUT_GENERAL, 1, &rg);
        barrier(g_in.ci, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_SHADER_READ_BIT);
        VkMemoryBarrier cb2{VK_STRUCTURE_TYPE_MEMORY_BARRIER}; cb2.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; cb2.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        dev->CmdPipelineBarrier(c, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &cb2, 0, nullptr, 0, nullptr);
        use_ci = own_c; use_cv = own_cv;
    }
    // debug variant: neutral layer-owned depth (0.5) and motion (0) instead of BeamNG's images
    static VkImage own_d = VK_NULL_HANDLE, own_m = VK_NULL_HANDLE; static VkImageView own_dv = VK_NULL_HANDLE, own_mv = VK_NULL_HANDLE;
    static const bool use_own_dm = getenv("FSR4STUDY_OWN_DEPTH_MV") != nullptr;
    VkImage use_di = g_in.di, use_mi = g_in.mi; VkImageView use_dv = g_in.dv, use_mvv = g_in.mv;
    if (use_own_dm) {
        auto make = [&](VkFormat fmt, VkImageUsageFlags usage, VkImageAspectFlags asp, VkImage* img, VkImageView* view) {
            VkImageCreateInfo ic{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO}; ic.imageType = VK_IMAGE_TYPE_2D; ic.format = fmt; ic.extent = {g_in.cinf.w, g_in.cinf.h, 1}; ic.mipLevels = 1; ic.arrayLayers = 1; ic.samples = VK_SAMPLE_COUNT_1_BIT; ic.tiling = VK_IMAGE_TILING_OPTIMAL; ic.usage = usage;
            dev->CreateImage(g_device, &ic, nullptr, img);
            VkMemoryRequirements req; dev->GetImageMemoryRequirements(g_device, *img, &req);
            auto gpdmp = reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(g_gipa(g_instance, "vkGetPhysicalDeviceMemoryProperties")); VkPhysicalDeviceMemoryProperties mp; gpdmp(g_pd, &mp);
            uint32_t ti = 0; for (uint32_t i = 0; i < mp.memoryTypeCount; ++i) if ((req.memoryTypeBits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) { ti = i; break; }
            VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; ai.allocationSize = req.size; ai.memoryTypeIndex = ti; VkDeviceMemory m; dev->AllocateMemory(g_device, &ai, nullptr, &m); dev->BindImageMemory(g_device, *img, m, 0);
            VkImageViewCreateInfo vc{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO}; vc.image = *img; vc.viewType = VK_IMAGE_VIEW_TYPE_2D; vc.format = fmt; vc.subresourceRange = {asp, 0, 1, 0, 1};
            reinterpret_cast<PFN_vkCreateImageView>(dev->gdpa(g_device, "vkCreateImageView"))(g_device, &vc, nullptr, view);
            barrier(*img, asp, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
        };
        if (!own_d) {
            make(VK_FORMAT_D32_SFLOAT, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_ASPECT_DEPTH_BIT, &own_d, &own_dv);
            make(VK_FORMAT_R16G16_SFLOAT, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_ASPECT_COLOR_BIT, &own_m, &own_mv);
            Ev("FSR4: using neutral layer-owned depth/motion");
        }
        VkClearDepthStencilValue half{0.5f, 0}; VkImageSubresourceRange dr{VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
        reinterpret_cast<PFN_vkCmdClearDepthStencilImage>(dev->gdpa(g_device, "vkCmdClearDepthStencilImage"))(c, own_d, VK_IMAGE_LAYOUT_GENERAL, &half, 1, &dr);
        VkClearColorValue zero{}; VkImageSubresourceRange cr{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        dev->CmdClearColorImage(c, own_m, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &cr);
        VkMemoryBarrier cb3{VK_STRUCTURE_TYPE_MEMORY_BARRIER}; cb3.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; cb3.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        dev->CmdPipelineBarrier(c, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &cb3, 0, nullptr, 0, nullptr);
        use_di = own_d; use_dv = own_dv; use_mi = own_m; use_mvv = own_mv;
    }
    Fsr411::Frame fr;
    fr.cmdbuf = c;
    fr.color = {use_ci, use_cv, g_in.cinf.w, g_in.cinf.h};
    fr.depth = {use_di, use_dv, g_in.dinf.w, g_in.dinf.h};
    fr.motion = {use_mi, use_mvv, g_in.minf.w, g_in.minf.h};
    fr.output = {oi, ov, oinf.w, oinf.h};
    if (getenv("FSR4STUDY_OWN_OUTPUT")) fr.output = {own_o, own_ov, oinf.w, oinf.h};
    fr.render_width = g_in.cinf.w; fr.render_height = g_in.cinf.h;
    fr.ultra_performance = (float(oinf.w) / float(g_in.cinf.w)) >= 2.9f;
    // A soft run has no trustworthy jitter; with reset set, a wrong guess affects only this frame.
    if (!soft) { fr.jitter[0] = g_f4.table[ph][0]; fr.jitter[1] = g_f4.table[ph][1]; }
    fr.motion_scale[0] = float(g_in.cinf.w); fr.motion_scale[1] = float(g_in.cinf.h);  // BeamNG velocity is in UV units
    fr.pre_exposure = 1.0f; fr.auto_exposure = true;
    fr.sharpen = g_f4_sharp >= 0.0f; fr.sharpness = g_f4_sharp > 0.0f ? g_f4_sharp : 0.0f;
    fr.reset = soft || g_f4.need_reset || g_f4.last_used + 1 != g_f4.counter;
    // Backend Create destroys the preceding model's images and UBO ring. A
    // size/model change must retire prior submitted work before Record does so.
    if (g_f4.used && (g_f4.backend_ow != oinf.w || g_f4.backend_oh != oinf.h || g_f4.backend_ultra != fr.ultra_performance)) {
        dev->DeviceWaitIdle(g_device);
        Ev("FSR4: retired previous backend resources before size/model change");
    }
    BeginUniformProbe();
    bool ok = g_f4.up->Record(fr);
    if (g_uniform_probe_active) { g_uniform_probe_active = false; g_uniform_probe.recorded = true; Ev("FSR4: uniform probe recorded passes=%u parts=%zu", g_uniform_probe_pass, g_uniform_probe.parts.size()); }
    if (!ok) { Ev("FSR4: Record failed: %s -> permanently fail-open", g_f4.up->Error().c_str()); g_f4.failed = true; }
    if (ok && getenv("FSR4STUDY_OWN_OUTPUT")) {
        barrier(own_o, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
        barrier(oi, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
        VkImageCopy rg{}; rg.srcSubresource = rg.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}; rg.extent = {oinf.w, oinf.h, 1};
        dev->CmdCopyImage(c, own_o, VK_IMAGE_LAYOUT_GENERAL, oi, VK_IMAGE_LAYOUT_GENERAL, 1, &rg);
        VkMemoryBarrier copied{VK_STRUCTURE_TYPE_MEMORY_BARRIER}; copied.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; copied.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
        dev->CmdPipelineBarrier(c, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &copied, 0, nullptr, 0, nullptr);
    }
    g_f4.need_reset = false; g_f4.last_used = g_f4.counter;
    // return inputs to the layouts BeamNG expects, make the output visible to later passes
    barrier(g_in.ci, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL, g_in.cl, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, rd, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_SHADER_READ_BIT);
    barrier(g_in.di, depth_all, VK_IMAGE_LAYOUT_GENERAL, g_in.dl, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, rd, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_SHADER_READ_BIT);
    barrier(g_in.mi, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL, g_in.ml, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, rd, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_SHADER_READ_BIT);
    VkMemoryBarrier ob{VK_STRUCTURE_TYPE_MEMORY_BARRIER}; ob.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT; ob.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_MEMORY_READ_BIT;
    dev->CmdPipelineBarrier(c, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &ob, 0, nullptr, 0, nullptr);
    // Push descriptors disturb the engine's set 0. Restore RCAS state even if
    // Record fails, so the original dispatch really is a valid fallback.
    dev->CmdBindPipeline(c, VK_PIPELINE_BIND_POINT_COMPUTE, (VkPipeline)game_state.compute_pipe);
    VkDescriptorSet game_set = (VkDescriptorSet)game_state.set0;
    dev->CmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_COMPUTE, (VkPipelineLayout)game_pipe.layout, 0, 1, &game_set, 0, nullptr);
    { std::lock_guard<std::mutex> lock(g_mutex); g_cbs[(uint64_t)c].compute_pipe = game_state.compute_pipe; }
    if (!ok) return false;
    g_f4.backend_ow = oinf.w; g_f4.backend_oh = oinf.h; g_f4.backend_ultra = fr.ultra_performance;
    if (!g_f4.why.empty()) { Ev("FSR4: resumed at counter=%llu after %s", (unsigned long long)g_f4.counter, g_f4.why.c_str()); g_f4.why.clear(); }
    if (soft) {
        ++g_skip.soft; g_skip.last_soft = true;
        Ev("FSR4: soft run on skipped FSR 3 chain at counter=%llu (%s), reset", (unsigned long long)g_f4.counter, soft);
    } else {
        g_skip.last_soft = false;
        g_f4.predictions[g_f4.counter % kRbSlots] = {g_f4.counter, fr.jitter[0], fr.jitter[1], true};
    }
    if (g_dump_active) Ev("FSR4: dump frame=%llu counter=%llu frameIdx=%lld jitter=[%.9g %.9g] reset=%u", (unsigned long long)g_frame.load(), (unsigned long long)g_f4.counter, (long long)idx, fr.jitter[0], fr.jitter[1], unsigned(fr.reset));
    if (++g_f4.used % 600 == 1) Ev("FSR4: frames replaced %llu, fail-open %llu (counter %llu) skippedChains %llu skippedDispatches %llu soft %llu hard %llu", (unsigned long long)g_f4.used, (unsigned long long)g_f4.failopen, (unsigned long long)g_f4.counter,
                                   (unsigned long long)g_skip.chains, (unsigned long long)g_skip.dispatches, (unsigned long long)g_skip.soft, (unsigned long long)g_skip.hard);
    return true;
}
// Same as InjectCbReadback but keyed by the layer's own frame counter (needed to align with frameIdx).
void F4InjectCbReadback(VkCommandBuffer c) {
    uint64_t set; { std::lock_guard<std::mutex> l(g_mutex); set = g_cbs[(uint64_t)c].set0; }
    Desc d{}; { std::lock_guard<std::mutex> l(g_mutex); auto it = g_dsets.find(set); if (it == g_dsets.end()) return; auto e = it->second.find(uint64_t(16) << 32); if (e == it->second.end()) return; d = e->second; }
    if (!d.view_or_buf || !EnsureReadback()) return;
    Dev* dev = D(c); int slot = int(g_f4.counter % kRbSlots);
    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER}; mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT; mb.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    dev->CmdPipelineBarrier(c, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
    VkBufferCopy r{d.off, uint64_t(slot) * kRbBytes, d.range < kRbBytes ? d.range : kRbBytes};
    dev->CmdCopyBuffer(c, (VkBuffer)d.view_or_buf, g_rb, 1, &r);
    g_f4.rb_counter[slot] = g_f4.counter;
}
VKAPI_ATTR void VKAPI_CALL L_CmdDispatch(VkCommandBuffer c, uint32_t x, uint32_t y, uint32_t z) {
    if (g_uniform_probe_active) {
        D(c)->CmdDispatch(c, x, y, z);
        Ev("FSR4: uniform probe dispatch=%u grid=%ux%ux%u", g_uniform_probe_pass, x, y, z);
        ProbeUniforms(c); ++g_uniform_probe_pass;
        return;
    }
    if (g_capturing) { uint64_t p; { std::lock_guard<std::mutex> l(g_mutex); p = g_cbs[(uint64_t)c].compute_pipe; } FlushDraws(c); Fr("cb%llx DISPATCH %u %u %u  (%s)", (unsigned long long)c, x, y, z, PipeStr(p).c_str()); DumpBoundSet(c); }
    static const bool f4mode = getenv("FSR4STUDY_FSR4") != nullptr;
    const uint32_t pf = CurPipeFlags(c);
    if (f4mode) {
        InitSharpness();
        if (pf & kPfUpscale) ChainTimerBegin(c);
        if (pf & kPfPrepare) { g_in.ok = false; if (KnownFsrShader(c, {0x731ab603aeae09afull})) F4CaptureInputs(c); }
        else if (pf & kPfAccumulate) { ++g_f4.counter; if (KnownFsrShader(c, {0x3f70673d45a7efcbull, 0xcd2a8432c2c24191ull})) F4InjectCbReadback(c); else g_in.ok = false; }
        else if (pf & kPfRcas) {
            if (g_dump_active) DumpRcasInput(c);
            F4InjectRcasReadback(c);
            const bool replaced = F4TryReplaceRcas(c);
            if (replaced) {
                ChainTimerEnd(c, true); g_skip.chain_open = false; g_skip.skip = false;
                if (g_dump_active) DumpOutput(c);
                return;
            }
            g_skip.chain_open = false; g_skip.skip = false;
        }
        if (pf & kPfSkippable) {
            // One decision per frame's chain; a new present means a new chain even without RCAS.
            if (!g_skip.chain_open || g_skip.chain_frame != g_frame) {
                g_skip.chain_open = true; g_skip.chain_frame = g_frame; g_skip.skip = F4ShouldSkipChain();
                if (g_skip.skip) ++g_skip.chains;
            }
            // Inputs and the CB readback were taken above; FSR 4 needs nothing this dispatch writes.
            if (g_skip.skip) {
                // Dump slots complete only with inputs (and exposure) captured on these dispatches.
                if (g_dump_active && (pf & kPfPrepare)) DumpInputs(c);
                if (g_dump_active && (pf & kPfAccumulate)) DumpExposure(c);
                ++g_skip.dispatches; return;
            }
        }
    }
    const bool acc = pf & kPfAccumulate, is_rcas = pf & kPfRcas;
    static const bool cb_readback = getenv("FSR4STUDY_CB_READBACK") != nullptr;
    if (acc && cb_readback) InjectCbReadback(c);
    static const bool subst = getenv("FSR4STUDY_SUBSTITUTE_RCAS") != nullptr;
    if (is_rcas && g_dump_active && !f4mode) DumpRcasInput(c);  // f4mode already copied it above
    if (is_rcas && subst) { SubstituteRcas(c); if (g_dump_active) DumpOutput(c); return; }
    if (g_dump_active && (pf & kPfPrepare)) DumpInputs(c);
    if (g_dump_active && acc) DumpExposure(c);
    D(c)->CmdDispatch(c, x, y, z);
    if (f4mode && g_timer.open && is_rcas) ChainTimerEnd(c, false);
    if (g_dump_active && is_rcas) DumpOutput(c);
}
VKAPI_ATTR void VKAPI_CALL L_CmdDispatchIndirect(VkCommandBuffer c, VkBuffer b, VkDeviceSize o) {
    Fr("cb%llx DISPATCH_INDIRECT", (unsigned long long)c); D(c)->CmdDispatchIndirect(c, b, o);
}
VKAPI_ATTR void VKAPI_CALL L_CmdPushConstants(VkCommandBuffer c, VkPipelineLayout l, VkShaderStageFlags st, uint32_t off, uint32_t size, const void* v) {
    if (g_capturing && (st & VK_SHADER_STAGE_COMPUTE_BIT)) {
        std::string h; const uint32_t* w = static_cast<const uint32_t*>(v);
        for (uint32_t i = 0; i < size / 4 && i < 64; ++i) { char b[12]; snprintf(b, sizeof b, "%08x ", w[i]); h += b; }
        Fr("cb%llx PUSH_CONSTANTS layout=%llx off=%u size=%u %s", (unsigned long long)c, (unsigned long long)l, off, size, h.c_str());
    }
    D(c)->CmdPushConstants(c, l, st, off, size, v);
}
VKAPI_ATTR void VKAPI_CALL L_CmdPushDescriptorSet(VkCommandBuffer c, VkPipelineBindPoint bp, VkPipelineLayout l, uint32_t set, uint32_t n, const VkWriteDescriptorSet* w) {
    if (g_uniform_probe_active && bp == VK_PIPELINE_BIND_POINT_COMPUTE) {
        g_uniform_descriptors.clear();
        for (uint32_t i = 0; i < n; ++i) if (w[i].descriptorType == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER && w[i].descriptorCount == 1 && w[i].pBufferInfo) g_uniform_descriptors[w[i].dstBinding] = *w[i].pBufferInfo;
    }
    if (g_capturing && bp == VK_PIPELINE_BIND_POINT_COMPUTE) {
        for (uint32_t i = 0; i < n; ++i) {
            for (uint32_t k = 0; k < w[i].descriptorCount; ++k) {
                std::string what;
                if (w[i].pImageInfo && (w[i].descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE || w[i].descriptorType == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE || w[i].descriptorType == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER))
                    what = ViewStr((uint64_t)w[i].pImageInfo[k].imageView) + " layout=" + std::to_string(int(w[i].pImageInfo[k].imageLayout));
                else if (w[i].pBufferInfo) what = "buffer " + Hex((uint64_t)w[i].pBufferInfo[k].buffer) + " off=" + std::to_string(w[i].pBufferInfo[k].offset) + " range=" + std::to_string(w[i].pBufferInfo[k].range);
                Fr("cb%llx PUSH_DESC set=%u binding=%u[%u] type=%d %s", (unsigned long long)c, set, w[i].dstBinding, w[i].dstArrayElement + k, int(w[i].descriptorType), what.c_str());
            }
        }
    }
    D(c)->CmdPushDescriptorSetKHR(c, bp, l, set, n, w);
}
VKAPI_ATTR void VKAPI_CALL L_CmdBindDescriptorSets(VkCommandBuffer c, VkPipelineBindPoint bp, VkPipelineLayout l, uint32_t first, uint32_t n, const VkDescriptorSet* s, uint32_t dn, const uint32_t* dyn) {
    if (bp == VK_PIPELINE_BIND_POINT_COMPUTE && first == 0 && n) { std::lock_guard<std::mutex> l(g_mutex); g_cbs[(uint64_t)c].set0 = (uint64_t)s[0]; }
    if (g_capturing && bp == VK_PIPELINE_BIND_POINT_COMPUTE) {
        std::string t; for (uint32_t i = 0; i < n; ++i) t += Hex((uint64_t)s[i]) + ",";
        std::string dd; for (uint32_t i = 0; i < dn; ++i) dd += std::to_string(dyn[i]) + ",";
        Fr("cb%llx BIND_DESC_SETS(compute) first=%u sets=%s dynoff=%s", (unsigned long long)c, first, t.c_str(), dd.c_str());
    }
    D(c)->CmdBindDescriptorSets(c, bp, l, first, n, s, dn, dyn);
}
// The bindings FSR 4 and the dumps read: cbFSR3Upscaler, cbRCAS, the output and the three scene inputs.
bool LeanBinding(uint32_t b) { return b == 16 || b == 17 || b == 31 || b == 47 || b == 48 || b == 49; }
void ShadowWrite(const VkWriteDescriptorSet& w) {
    if (g_lean && !LeanBinding(w.dstBinding)) return;
    std::lock_guard<std::mutex> l(g_mutex);
    auto& set = g_dsets[(uint64_t)w.dstSet];
    for (uint32_t k = 0; k < w.descriptorCount; ++k) {
        Desc d{int(w.descriptorType), 0, 0, 0, 0, 0};
        switch (w.descriptorType) {
        case VK_DESCRIPTOR_TYPE_SAMPLER: d.sampler = (uint64_t)w.pImageInfo[k].sampler; break;
        case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER: d.sampler = (uint64_t)w.pImageInfo[k].sampler; [[fallthrough]];
        case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE: case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE: case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT:
            d.view_or_buf = (uint64_t)w.pImageInfo[k].imageView; d.layout = int(w.pImageInfo[k].imageLayout); break;
        case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER: case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
        case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC: case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC:
            d.view_or_buf = (uint64_t)w.pBufferInfo[k].buffer; d.off = w.pBufferInfo[k].offset; d.range = w.pBufferInfo[k].range; break;
        default: break;  // texel buffers, acceleration structures: not needed for the upscaler
        }
        set[(uint64_t(w.dstBinding) << 32) | (w.dstArrayElement + k)] = d;
    }
}
std::string BufferContents(uint64_t buf, uint64_t off, uint64_t range) {
    BufInfo b; char* base = nullptr;
    { std::lock_guard<std::mutex> l(g_mutex); auto it = g_bufs.find(buf); if (it == g_bufs.end()) return "buf?"; b = it->second; auto m = g_mapped.find(b.mem); if (m != g_mapped.end()) base = m->second; }
    std::vector<uint8_t> shadow;
    { std::lock_guard<std::mutex> l(g_mutex);
      auto it = g_bufdata.find(buf); if (it != g_bufdata.end()) shadow = it->second;
      if (shadow.empty()) {
        uint64_t abs = b.off + off;
        auto m = g_memdata.upper_bound({b.mem, abs});
        if (m != g_memdata.begin()) { --m; if (m->first.first == b.mem && m->first.second <= abs && abs < m->first.second + m->second.size()) shadow.assign(m->second.begin() + (abs - m->first.second), m->second.end()); }
      } }
    char hdr[160]; snprintf(hdr, sizeof hdr, "buf=%llx size=%llu mem=%llx+%llu off=%llu range=%llu %s", (unsigned long long)buf, (unsigned long long)b.size, (unsigned long long)b.mem, (unsigned long long)b.off, (unsigned long long)off, (unsigned long long)range, !shadow.empty() ? "SHADOWED" : base ? "MAPPED" : "not-mapped");
    std::string s = hdr;
    if (shadow.empty()) {
        std::lock_guard<std::mutex> l(g_mutex); size_t cnt = 0; uint64_t lo = 0, hi = 0;
        for (auto& kv : g_memdata) if (kv.first.first == b.mem) { ++cnt; uint64_t o2 = kv.first.second; if (o2 <= b.off + off) lo = o2; else if (!hi) hi = o2; }
        char t[200]; snprintf(t, sizeof t, " [memdata entries for mem=%zu nearest<=%llu nearest>%llu]", cnt, (unsigned long long)lo, (unsigned long long)hi); s += t;
    }
    if (!shadow.empty() || base) {
        const uint32_t* w = !shadow.empty() ? reinterpret_cast<const uint32_t*>(shadow.data() + (off < shadow.size() ? off : 0)) : reinterpret_cast<const uint32_t*>(base + b.off + off);
        uint64_t n = (range == VK_WHOLE_SIZE ? 256 : range) / 4; if (n > 48) n = 48;
        s += " data=";
        for (uint64_t i = 0; i < n; ++i) { char t[12]; snprintf(t, sizeof t, "%08x ", w[i]); s += t; }
    }
    return s;
}
void DumpBoundSet(VkCommandBuffer c) {
    uint64_t set;
    { std::lock_guard<std::mutex> l(g_mutex); set = g_cbs[(uint64_t)c].set0; }
    std::vector<std::pair<uint64_t, Desc>> v;
    { std::lock_guard<std::mutex> l(g_mutex); auto it = g_dsets.find(set); if (it != g_dsets.end()) for (auto& kv : it->second) v.push_back(kv); }
    Fr("cb%llx   SET0=%llx (%zu descriptors known)", (unsigned long long)c, (unsigned long long)set, v.size());
    for (auto& kv : v) {
        uint32_t binding = uint32_t(kv.first >> 32), elem = uint32_t(kv.first & 0xffffffff); const Desc& d = kv.second;
        std::string what;
        if (d.type == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE || d.type == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE || d.type == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER) what = ViewStr(d.view_or_buf) + " layout=" + LayoutNameFwd(d.layout);
        else if (d.type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER || d.type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC) what = BufferContents(d.view_or_buf, d.off, d.range);
        else if (d.type == VK_DESCRIPTOR_TYPE_SAMPLER) what = "sampler " + Hex(d.sampler);
        else what = "buffer " + Hex(d.view_or_buf);
        if (binding >= 16 && binding < 80) Fr("cb%llx   B%u[%u] type=%d %s", (unsigned long long)c, binding, elem, d.type, what.c_str());
    }
}
VKAPI_ATTR void VKAPI_CALL L_UpdateDescriptorSets(VkDevice d, uint32_t nw, const VkWriteDescriptorSet* w, uint32_t nc, const VkCopyDescriptorSet* cp) {
    for (uint32_t i = 0; i < nw; ++i) ShadowWrite(w[i]);
    if (nc) {
        std::lock_guard<std::mutex> l(g_mutex);
        for (uint32_t i = 0; i < nc; ++i)
            for (uint32_t k = 0; k < cp[i].descriptorCount; ++k) {
                if (g_lean && !LeanBinding(cp[i].dstBinding)) break;
                auto& src = g_dsets[(uint64_t)cp[i].srcSet]; auto it = src.find((uint64_t(cp[i].srcBinding) << 32) | (cp[i].srcArrayElement + k));
                if (it != src.end()) g_dsets[(uint64_t)cp[i].dstSet][(uint64_t(cp[i].dstBinding) << 32) | (cp[i].dstArrayElement + k)] = it->second;
            }
    }
    D(d)->UpdateDescriptorSets(d, nw, w, nc, cp);
}
const char* LayoutName(VkImageLayout l);
const char* LayoutName(VkImageLayout l);
const char* LayoutNameFwd(int l) { return LayoutName(VkImageLayout(l)); }
const char* LayoutName(VkImageLayout l) { return l == VK_IMAGE_LAYOUT_UNDEFINED ? "UNDEF" : l == VK_IMAGE_LAYOUT_GENERAL ? "GENERAL" : l == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL ? "SHADER_RO" : l == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL ? "COLOR_ATT" : l == VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL ? "DEPTH_ATT" : l == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL ? "XFER_SRC" : l == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL ? "XFER_DST" : l == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR ? "PRESENT" : "other"; }
VKAPI_ATTR void VKAPI_CALL L_CmdPipelineBarrier(VkCommandBuffer c, VkPipelineStageFlags s, VkPipelineStageFlags d, VkDependencyFlags f, uint32_t nm, const VkMemoryBarrier* m, uint32_t nb, const VkBufferMemoryBarrier* b, uint32_t ni, const VkImageMemoryBarrier* i) {
    if (g_capturing) {
        FlushDraws(c);
        Fr("cb%llx BARRIER src=%x dst=%x mem=%u buf=%u img=%u", (unsigned long long)c, unsigned(s), unsigned(d), nm, nb, ni);
        for (uint32_t k = 0; k < ni && k < 16; ++k) Fr("cb%llx    IMGBAR %s %s->%s", (unsigned long long)c, ImageStr((uint64_t)i[k].image).c_str(), LayoutName(i[k].oldLayout), LayoutName(i[k].newLayout));
    }
    D(c)->CmdPipelineBarrier(c, s, d, f, nm, m, nb, b, ni, i);
}
VKAPI_ATTR void VKAPI_CALL L_CmdPipelineBarrier2(VkCommandBuffer c, const VkDependencyInfo* di) {
    if (g_capturing) {
        FlushDraws(c);
        Fr("cb%llx BARRIER2 mem=%u buf=%u img=%u", (unsigned long long)c, di->memoryBarrierCount, di->bufferMemoryBarrierCount, di->imageMemoryBarrierCount);
        for (uint32_t k = 0; k < di->imageMemoryBarrierCount && k < 16; ++k) { auto& b = di->pImageMemoryBarriers[k]; Fr("cb%llx    IMGBAR2 %s %s->%s src=%llx/%llx dst=%llx/%llx", (unsigned long long)c, ImageStr((uint64_t)b.image).c_str(), LayoutName(b.oldLayout), LayoutName(b.newLayout), (unsigned long long)b.srcStageMask, (unsigned long long)b.srcAccessMask, (unsigned long long)b.dstStageMask, (unsigned long long)b.dstAccessMask); }
    }
    D(c)->CmdPipelineBarrier2(c, di);
}
VKAPI_ATTR void VKAPI_CALL L_CmdBeginRendering(VkCommandBuffer c, const VkRenderingInfo* ri) {
    if (g_capturing) {
        FlushDraws(c);
        Fr("cb%llx BEGIN_RENDERING area=%ux%u colors=%u depth=%d", (unsigned long long)c, ri->renderArea.extent.width, ri->renderArea.extent.height, ri->colorAttachmentCount, ri->pDepthAttachment ? 1 : 0);
        for (uint32_t k = 0; k < ri->colorAttachmentCount; ++k) Fr("cb%llx    COLOR%u %s", (unsigned long long)c, k, ViewStr((uint64_t)ri->pColorAttachments[k].imageView).c_str());
        if (ri->pDepthAttachment) Fr("cb%llx    DEPTH %s", (unsigned long long)c, ViewStr((uint64_t)ri->pDepthAttachment->imageView).c_str());
    }
    D(c)->CmdBeginRendering(c, ri);
}
VKAPI_ATTR void VKAPI_CALL L_CmdBeginRenderPass(VkCommandBuffer c, const VkRenderPassBeginInfo* bi, VkSubpassContents sc) { FlushDraws(c); Fr("cb%llx BEGIN_RENDERPASS area=%ux%u", (unsigned long long)c, bi->renderArea.extent.width, bi->renderArea.extent.height); D(c)->CmdBeginRenderPass(c, bi, sc); }
#define DRAWHOOK(NAME, SIG, CALL) VKAPI_ATTR void VKAPI_CALL L_##NAME SIG { if (g_capturing) { std::lock_guard<std::mutex> l(g_mutex); g_cbs[(uint64_t)c].draws++; } D(c)->NAME CALL; }
VKAPI_ATTR void VKAPI_CALL L_CmdDraw(VkCommandBuffer c, uint32_t a, uint32_t b, uint32_t e, uint32_t f) { if (g_capturing) { std::lock_guard<std::mutex> l(g_mutex); g_cbs[(uint64_t)c].draws++; } D(c)->CmdDraw(c, a, b, e, f); }
VKAPI_ATTR void VKAPI_CALL L_CmdDrawIndexed(VkCommandBuffer c, uint32_t a, uint32_t b, uint32_t e, int32_t f, uint32_t g) { if (g_capturing) { std::lock_guard<std::mutex> l(g_mutex); g_cbs[(uint64_t)c].draws++; } D(c)->CmdDrawIndexed(c, a, b, e, f, g); }
VKAPI_ATTR void VKAPI_CALL L_CmdDrawIndirect(VkCommandBuffer c, VkBuffer b, VkDeviceSize o, uint32_t n, uint32_t s) { if (g_capturing) { std::lock_guard<std::mutex> l(g_mutex); g_cbs[(uint64_t)c].draws++; } D(c)->CmdDrawIndirect(c, b, o, n, s); }
VKAPI_ATTR void VKAPI_CALL L_CmdDrawIndexedIndirect(VkCommandBuffer c, VkBuffer b, VkDeviceSize o, uint32_t n, uint32_t s) { if (g_capturing) { std::lock_guard<std::mutex> l(g_mutex); g_cbs[(uint64_t)c].draws++; } D(c)->CmdDrawIndexedIndirect(c, b, o, n, s); }
VKAPI_ATTR void VKAPI_CALL L_CmdDrawIndirectCount(VkCommandBuffer c, VkBuffer b, VkDeviceSize o, VkBuffer cb, VkDeviceSize co, uint32_t m, uint32_t s) { if (g_capturing) { std::lock_guard<std::mutex> l(g_mutex); g_cbs[(uint64_t)c].draws++; } D(c)->CmdDrawIndirectCount(c, b, o, cb, co, m, s); }
VKAPI_ATTR void VKAPI_CALL L_CmdDrawIndexedIndirectCount(VkCommandBuffer c, VkBuffer b, VkDeviceSize o, VkBuffer cb, VkDeviceSize co, uint32_t m, uint32_t s) { if (g_capturing) { std::lock_guard<std::mutex> l(g_mutex); g_cbs[(uint64_t)c].draws++; } D(c)->CmdDrawIndexedIndirectCount(c, b, o, cb, co, m, s); }
VKAPI_ATTR void VKAPI_CALL L_CmdCopyImage(VkCommandBuffer c, VkImage s, VkImageLayout sl, VkImage d, VkImageLayout dl, uint32_t n, const VkImageCopy* r) { FlushDraws(c); Fr("cb%llx COPY_IMAGE %s -> %s", (unsigned long long)c, ImageStr((uint64_t)s).c_str(), ImageStr((uint64_t)d).c_str()); D(c)->CmdCopyImage(c, s, sl, d, dl, n, r); }
VKAPI_ATTR void VKAPI_CALL L_CmdBlitImage(VkCommandBuffer c, VkImage s, VkImageLayout sl, VkImage d, VkImageLayout dl, uint32_t n, const VkImageBlit* r, VkFilter f) { FlushDraws(c); Fr("cb%llx BLIT_IMAGE %s -> %s", (unsigned long long)c, ImageStr((uint64_t)s).c_str(), ImageStr((uint64_t)d).c_str()); D(c)->CmdBlitImage(c, s, sl, d, dl, n, r, f); }
VKAPI_ATTR void VKAPI_CALL L_CmdCopyBufferToImage(VkCommandBuffer c, VkBuffer s, VkImage d, VkImageLayout dl, uint32_t n, const VkBufferImageCopy* r) { Fr("cb%llx COPY_BUF2IMG -> %s", (unsigned long long)c, ImageStr((uint64_t)d).c_str()); D(c)->CmdCopyBufferToImage(c, s, d, dl, n, r); }
VKAPI_ATTR void VKAPI_CALL L_CmdClearColorImage(VkCommandBuffer c, VkImage i, VkImageLayout l, const VkClearColorValue* v, uint32_t n, const VkImageSubresourceRange* r) { Fr("cb%llx CLEAR_IMAGE %s", (unsigned long long)c, ImageStr((uint64_t)i).c_str()); D(c)->CmdClearColorImage(c, i, l, v, n, r); }
VKAPI_ATTR VkResult VKAPI_CALL L_QueueSubmit(VkQueue q, uint32_t n, const VkSubmitInfo* s, VkFence f) {
    if (g_capturing) for (uint32_t i = 0; i < n; ++i) { std::string t; for (uint32_t k = 0; k < s[i].commandBufferCount; ++k) t += Hex((uint64_t)s[i].pCommandBuffers[k]) + ","; Fr("SUBMIT q=%llx waitSem=%u signalSem=%u cbs=%s", (unsigned long long)q, s[i].waitSemaphoreCount, s[i].signalSemaphoreCount, t.c_str()); }
    return D(q)->QueueSubmit(q, n, s, f);
}
VKAPI_ATTR VkResult VKAPI_CALL L_QueueSubmit2(VkQueue q, uint32_t n, const VkSubmitInfo2* s, VkFence f) {
    if (g_capturing) for (uint32_t i = 0; i < n; ++i) { std::string t; for (uint32_t k = 0; k < s[i].commandBufferInfoCount; ++k) t += Hex((uint64_t)s[i].pCommandBufferInfos[k].commandBuffer) + ","; Fr("SUBMIT2 q=%llx waitSem=%u signalSem=%u cbs=%s", (unsigned long long)q, s[i].waitSemaphoreInfoCount, s[i].signalSemaphoreInfoCount, t.c_str()); }
    return D(q)->QueueSubmit2(q, n, s, f);
}
// Status for the BeamNG menu-label mod, which cannot read the environment: rewritten about every 5 s into
// <BeamNG user folder>/temp/bng_fsr4/status.json (BNG_FSR4_USERFOLDER overrides the folder).
void WriteStatus() {
    static double last = -1e9; static std::string dir, path, tmp; static bool usable = true;
    if (!usable) return;
    timespec now; clock_gettime(CLOCK_MONOTONIC_COARSE, &now);
    const double t = double(now.tv_sec) + now.tv_nsec * 1e-9;
    if (t - last < 5.0) return;
    last = t;
    if (dir.empty()) {
        std::string user;
        if (const char* u = getenv("BNG_FSR4_USERFOLDER")) user = u;
        else { const char* data = getenv("XDG_DATA_HOME"); const char* home = getenv("HOME");
               user = (data && *data ? std::string(data) : std::string(home ? home : "") + "/.local/share") + "/BeamNG/BeamNG.drive/current"; }
        struct stat st; if (stat(user.c_str(), &st) != 0) { usable = false; Ev("STATUS: BeamNG user folder %s not found; menu label stays FSR 3", user.c_str()); return; }
        mkdir((user + "/temp").c_str(), 0755); dir = user + "/temp/bng_fsr4"; mkdir(dir.c_str(), 0755);
        path = dir + "/status.json"; tmp = path + ".tmp";
    }
    const bool ok = g_fsr4_features && !g_f4.failed;
    if (FILE* f = fopen(tmp.c_str(), "w")) {
        fprintf(f, "{\"layer\":\"bng-fsr4\",\"time\":%lld,\"ok\":%s,\"replaced\":%llu,\"failopen\":%llu}\n", (long long)::time(nullptr),
                ok ? "true" : "false", (unsigned long long)g_f4.used, (unsigned long long)g_f4.failopen);
        fclose(f); rename(tmp.c_str(), path.c_str());
    }
}
VKAPI_ATTR VkResult VKAPI_CALL L_QueuePresentKHR(VkQueue q, const VkPresentInfoKHR* p) {
    static const bool f4mode_status = getenv("FSR4STUDY_FSR4") != nullptr;
    if (f4mode_status) WriteStatus();
    Fr("PRESENT");
    uint64_t f = ++g_frame;
    if (g_lean) {
        if (f % 8 == 1) PollOutFiles();
        else if (!g_dump_active || g_dump_left > 0) return D(q)->QueuePresentKHR(q, p);  // dump.trigger is polled on the same frames
    }
    if (f % 4 == 0 && !getenv("FSR4STUDY_FSR4")) DrainReadback();
    { struct stat st; std::string t = g_out + "/dump.trigger";
      if (!g_dump_active && stat(t.c_str(), &st) == 0) { unlink(t.c_str()); if (const char* n = getenv("FSR4STUDY_DUMP_FRAMES")) g_dump_total = atoi(n); g_dump_left = g_dump_total; g_dump_next = 0; g_dump_slot_bytes = 0; g_dump_active = true; Ev("DUMP armed for %d frames at frame %llu", g_dump_total, (unsigned long long)f); }
      else if (g_dump_active && g_dump_left <= 0) FinishDump(); }
    if (g_capturing && --g_capture_left <= 0) { g_capturing = false; Ev("CAPTURE end at frame %llu (cmdUpdateBuffer=%llu cmdCopyBuffer regions=%llu from-mapped=%llu)", (unsigned long long)f, (unsigned long long)g_upd_cnt.load(), (unsigned long long)g_copy_cnt.load(), (unsigned long long)g_copy_mapped.load()); if (g_frames) fflush(g_frames); }
    if (!g_lean) MaybeStartCapture();
    return D(q)->QueuePresentKHR(q, p);
}

// Copy descriptor-sized UBO ranges after each dispatch of one warm backend frame.
// Source usage is augmented only for backend buffers and only when the probe is enabled.
void BeginUniformProbe() {
    if (!getenv("FSR4STUDY_UNIFORMS") || g_uniform_probe.recorded || g_uniform_probe.buffer || g_f4.used != 32) return;
    auto* dev = D(g_device);
    constexpr VkDeviceSize capacity = 64u << 10;
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO}; bi.size = capacity; bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    if (dev->CreateBuffer(g_device, &bi, nullptr, &g_uniform_probe.buffer) != VK_SUCCESS) return;
    VkMemoryRequirements req; dev->GetBufferMemoryRequirements(g_device, g_uniform_probe.buffer, &req);
    VkPhysicalDeviceMemoryProperties mp; reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(g_gipa(g_instance, "vkGetPhysicalDeviceMemoryProperties"))(g_pd, &mp);
    uint32_t ti = UINT32_MAX;
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i) if ((req.memoryTypeBits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) == (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) { ti = i; break; }
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; ai.allocationSize = req.size; ai.memoryTypeIndex = ti; VkDeviceMemory memory;
    if (ti == UINT32_MAX || dev->AllocateMemory(g_device, &ai, nullptr, &memory) != VK_SUCCESS || dev->BindBufferMemory(g_device, g_uniform_probe.buffer, memory, 0) != VK_SUCCESS) return;
    void* ptr = nullptr; if (dev->MapMemory(g_device, memory, 0, VK_WHOLE_SIZE, 0, &ptr) != VK_SUCCESS) return;
    g_uniform_probe.mapped = static_cast<uint8_t*>(ptr); memset(ptr, 0xee, capacity);
    g_uniform_probe.counter = g_f4.counter;
    g_uniform_probe_active = true; g_uniform_probe_pass = 0; g_uniform_descriptors.clear();
}
void ProbeUniforms(VkCommandBuffer c) {
    auto* dev = D(c);
    VkMemoryBarrier before{VK_STRUCTURE_TYPE_MEMORY_BARRIER}; before.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_MEMORY_READ_BIT; before.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    dev->CmdPipelineBarrier(c, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &before, 0, nullptr, 0, nullptr);
    for (auto& [binding, info] : g_uniform_descriptors) {
        BufInfo b; char* mapped = nullptr;
        { std::lock_guard<std::mutex> lock(g_mutex); auto it = g_bufs.find((uint64_t)info.buffer); if (it != g_bufs.end()) { b = it->second; auto m = g_mapped.find(b.mem); if (m != g_mapped.end()) mapped = m->second; } }
        VkDeviceSize bytes = info.range == VK_WHOLE_SIZE && info.offset < b.size ? b.size - info.offset : info.range;
        if (!mapped || !bytes || bytes > 512 || info.offset > b.size || bytes > b.size - info.offset || g_uniform_probe.cursor + bytes > (64u << 10)) { Ev("FSR4: uniform probe cannot snapshot binding=%u", binding); continue; }
        UniformProbePart part{g_uniform_probe_pass, binding, g_uniform_probe.cursor, {}};
        auto* src = reinterpret_cast<uint8_t*>(mapped) + b.off + info.offset;
        part.expected.assign(src, src + bytes);
        VkBufferCopy copy{info.offset, part.offset, bytes}; dev->CmdCopyBuffer(c, info.buffer, g_uniform_probe.buffer, 1, &copy);
        g_uniform_probe.parts.push_back(std::move(part)); g_uniform_probe.cursor += (bytes + 255) & ~VkDeviceSize(255);
    }
    VkMemoryBarrier after{VK_STRUCTURE_TYPE_MEMORY_BARRIER}; after.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT; after.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_UNIFORM_READ_BIT | VK_ACCESS_HOST_READ_BIT;
    dev->CmdPipelineBarrier(c, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &after, 0, nullptr, 0, nullptr);
}
void FinishUniformProbe() {
    if (!g_uniform_probe.recorded || g_uniform_probe.checked || g_f4.counter < g_uniform_probe.counter + 16) return;
    D(g_device)->DeviceWaitIdle(g_device);
    std::string dir = g_out + "/uniform-probe"; mkdir(dir.c_str(), 0755);
    for (auto& part : g_uniform_probe.parts) {
        auto* actual = g_uniform_probe.mapped + part.offset;
        size_t different = 0, nonzero = 0;
        for (size_t i = 0; i < part.expected.size(); ++i) { different += actual[i] != part.expected[i]; nonzero += actual[i] != 0; }
        Ev("FSR4: uniform readback pass=%u binding=%u bytes=%zu different=%zu nonzero=%zu", part.pass, part.binding, part.expected.size(), different, nonzero);
        char name[512]; snprintf(name, sizeof name, "%s/pass%02u_binding%u_gpu.raw", dir.c_str(), part.pass, part.binding);
        if (FILE* f = fopen(name, "wb")) { fwrite(actual, 1, part.expected.size(), f); fclose(f); }
        snprintf(name, sizeof name, "%s/pass%02u_binding%u_cpu.raw", dir.c_str(), part.pass, part.binding);
        if (FILE* f = fopen(name, "wb")) { fwrite(part.expected.data(), 1, part.expected.size(), f); fclose(f); }
    }
    g_uniform_probe.checked = true;
}

// ---- proc-addr plumbing --------------------------------------------------------------------------
struct Hook { const char* name; PFN_vkVoidFunction fn; };
#define H(n) {"vk" #n, (PFN_vkVoidFunction)L_##n}
const Hook kDevHooks[] = {
    H(CreateShaderModule), H(CreateComputePipelines), H(CreateGraphicsPipelines), H(CreateImage), H(CreateImageView),
    H(CreatePipelineLayout), H(CreateDescriptorSetLayout), H(SetDebugUtilsObjectNameEXT), H(BeginCommandBuffer),
    H(CmdBindPipeline), H(CmdDispatch), H(CmdDispatchIndirect), H(CmdPushConstants), H(CmdBindDescriptorSets),
    H(UpdateDescriptorSets), H(CmdPipelineBarrier), H(CmdPipelineBarrier2), {"vkCmdPipelineBarrier2KHR", (PFN_vkVoidFunction)L_CmdPipelineBarrier2},
    H(CmdBeginRendering), {"vkCmdBeginRenderingKHR", (PFN_vkVoidFunction)L_CmdBeginRendering}, H(CmdBeginRenderPass),
    H(CmdDraw), H(CmdDrawIndexed), H(CmdDrawIndirect), H(CmdDrawIndexedIndirect), H(CmdDrawIndirectCount), H(CmdDrawIndexedIndirectCount),
    H(CreateBuffer), H(AllocateMemory), H(BindBufferMemory), H(BindBufferMemory2), {"vkBindBufferMemory2KHR", (PFN_vkVoidFunction)L_BindBufferMemory2}, H(MapMemory), H(CmdUpdateBuffer), H(CmdCopyBuffer), H(CmdCopyBuffer2), {"vkCmdCopyBuffer2KHR", (PFN_vkVoidFunction)L_CmdCopyBuffer2}, H(MapMemory2), {"vkMapMemory2KHR", (PFN_vkVoidFunction)L_MapMemory2}, H(UpdateDescriptorSetWithTemplate), {"vkUpdateDescriptorSetWithTemplateKHR", (PFN_vkVoidFunction)L_UpdateDescriptorSetWithTemplate}, H(CmdCopyImage), H(CmdBlitImage), H(CmdCopyBufferToImage), H(CmdClearColorImage),
    H(QueueSubmit), H(QueueSubmit2), {"vkQueueSubmit2KHR", (PFN_vkVoidFunction)L_QueueSubmit2}, H(QueuePresentKHR),
    {"vkCmdBeginDebugUtilsLabelEXT", (PFN_vkVoidFunction)L_CmdBeginLabel}, {"vkCmdEndDebugUtilsLabelEXT", (PFN_vkVoidFunction)L_CmdEndLabel},
    {"vkCmdInsertDebugUtilsLabelEXT", (PFN_vkVoidFunction)L_CmdInsertLabel}, {"vkCmdPushDescriptorSetKHR", (PFN_vkVoidFunction)L_CmdPushDescriptorSet},
    {"vkCmdPushDescriptorSet", (PFN_vkVoidFunction)L_CmdPushDescriptorSet},
};
// Everything else goes straight to the driver in lean mode (draws, copies, barriers, labels, submits, ...).
const char* const kLeanHooks[] = {"vkCreateShaderModule", "vkCreateComputePipelines", "vkCreateImage", "vkCreateImageView",
    "vkSetDebugUtilsObjectNameEXT", "vkBeginCommandBuffer", "vkCmdBindPipeline", "vkCmdDispatch", "vkCmdBindDescriptorSets",
    "vkUpdateDescriptorSets", "vkCreateBuffer", "vkQueuePresentKHR"};
PFN_vkVoidFunction FindHook(const char* n) {
    if (g_inert) return nullptr;
    if (g_lean && std::none_of(std::begin(kLeanHooks), std::end(kLeanHooks), [n](const char* k) { return !strcmp(k, n); })) return nullptr;
    for (auto& h : kDevHooks) if (!strcmp(h.name, n)) return h.fn;
    return nullptr;
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL L_GetDeviceProcAddr(VkDevice d, const char* name) {
    Dev* dev = D(d);
    if (!dev) return nullptr;
    PFN_vkVoidFunction real = dev->gdpa(d, name);
    if (!real) return nullptr;
    PFN_vkVoidFunction h = FindHook(name);
    return h ? h : real;
}
VKAPI_ATTR VkResult VKAPI_CALL L_CreateDevice(VkPhysicalDevice pd, const VkDeviceCreateInfo* ci, const VkAllocationCallbacks* a, VkDevice* out) {
    auto* chain = const_cast<VkLayerDeviceCreateInfo*>(reinterpret_cast<const VkLayerDeviceCreateInfo*>(ci->pNext));
    while (chain && !(chain->sType == VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO && chain->function == VK_LAYER_LINK_INFO)) chain = const_cast<VkLayerDeviceCreateInfo*>(reinterpret_cast<const VkLayerDeviceCreateInfo*>(chain->pNext));
    if (!chain) return VK_ERROR_INITIALIZATION_FAILED;
    PFN_vkGetInstanceProcAddr gipa = chain->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    PFN_vkGetDeviceProcAddr gdpa = chain->u.pLayerInfo->pfnNextGetDeviceProcAddr;
    auto* saved_link = chain->u.pLayerInfo;
    // Restore the caller's feature request on every exit, and before fallback.
    struct FeatureEdits {
        std::vector<std::pair<VkBool32*, VkBool32>> saved;
        void Set(VkBool32& field, VkBool32 value) { saved.emplace_back(&field, field); field = value; }
        void Restore() { for (auto it = saved.rbegin(); it != saved.rend(); ++it) *it->first = it->second; saved.clear(); }
        ~FeatureEdits() { Restore(); }
    } edits;
    chain->u.pLayerInfo = chain->u.pLayerInfo->pNext;
    auto create = reinterpret_cast<PFN_vkCreateDevice>(gipa(VK_NULL_HANDLE, "vkCreateDevice"));
    VkResult r = VK_ERROR_INITIALIZATION_FAILED;
    g_fsr4_features = false;
    if (getenv("FSR4STUDY_FSR4")) {
        // Enable what FSR 4 INT8 needs on top of what the game asked for; fall back to the untouched request on failure.
        auto* mut = const_cast<VkBaseOutStructure*>(reinterpret_cast<const VkBaseOutStructure*>(ci->pNext));
        bool had12 = false, had13 = false;
        VkPhysicalDeviceVulkan13Features supported13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
        VkPhysicalDeviceVulkan12Features supported12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, &supported13};
        VkPhysicalDeviceVulkan11Features supported11{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, &supported12};
        VkPhysicalDeviceFeatures2 supported{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &supported11};
        auto query = reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures2>(gipa(g_instance, "vkGetPhysicalDeviceFeatures2"));
        if (query) query(pd, &supported);
        std::string probe = getenv("FSR4STUDY_FEATURES") ? getenv("FSR4STUDY_FEATURES") : "";
        const bool storage = probe == "storage" || probe == "bench";
        const bool scalar = probe == "scalar" || probe == "bench";
        const bool subgroup = probe == "subgroup" || probe == "bench";
        // Required wave sizes per FSR 4 pass (Vulkan 1.3 core); enabling the feature does not change the game's pipelines.
        bool wave_ok = false;
        if (auto qp = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(gipa(g_instance, "vkGetPhysicalDeviceProperties2"))) {
            VkPhysicalDeviceSubgroupSizeControlProperties sizes{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_PROPERTIES};
            VkPhysicalDeviceProperties2 props{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &sizes}; qp(pd, &props);
            wave_ok = supported13.subgroupSizeControl && (sizes.requiredSubgroupSizeStages & VK_SHADER_STAGE_COMPUTE_BIT) &&
                      sizes.minSubgroupSize <= 32 && sizes.maxSubgroupSize >= 64 && !getenv("FSR4STUDY_DEFAULT_WAVE");
        }
        Ev("FSR4: feature probe=%s supported: storage16=%u uniformStorage16=%u storage8=%u uniformStorage8=%u scalar=%u subgroup=%u fullSubgroups=%u", probe.c_str(), supported11.storageBuffer16BitAccess, supported11.uniformAndStorageBuffer16BitAccess, supported12.storageBuffer8BitAccess, supported12.uniformAndStorageBuffer8BitAccess, supported12.scalarBlockLayout, supported13.subgroupSizeControl, supported13.computeFullSubgroups);
        for (auto* p = mut; p; p = p->pNext) {
            if (p->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES) { auto* f = reinterpret_cast<VkPhysicalDeviceVulkan12Features*>(p); edits.Set(f->shaderInt8, VK_TRUE); had12 = true;
                if (storage) { edits.Set(f->storageBuffer8BitAccess, supported12.storageBuffer8BitAccess); edits.Set(f->uniformAndStorageBuffer8BitAccess, supported12.uniformAndStorageBuffer8BitAccess); }
                if (scalar) edits.Set(f->scalarBlockLayout, supported12.scalarBlockLayout); }
            if (p->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES) { auto* f = reinterpret_cast<VkPhysicalDeviceVulkan13Features*>(p); edits.Set(f->shaderIntegerDotProduct, VK_TRUE); had13 = true;
                if (subgroup) { edits.Set(f->subgroupSizeControl, supported13.subgroupSizeControl); edits.Set(f->computeFullSubgroups, supported13.computeFullSubgroups); }
                else if (wave_ok) edits.Set(f->subgroupSizeControl, VK_TRUE);
                if (probe == "bench") edits.Set(f->robustImageAccess, VK_FALSE); }
        }
        for (auto* p = mut; p; p = p->pNext) {
            if (p->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES && storage) { auto* f = reinterpret_cast<VkPhysicalDeviceVulkan11Features*>(p); edits.Set(f->storageBuffer16BitAccess, supported11.storageBuffer16BitAccess); edits.Set(f->uniformAndStorageBuffer16BitAccess, supported11.uniformAndStorageBuffer16BitAccess); }
            if (p->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 && probe == "bench") { auto* f = reinterpret_cast<VkPhysicalDeviceFeatures2*>(p); edits.Set(f->features.robustBufferAccess, VK_FALSE); }
        }
        bool valve_ok = false;
        if (had12 && had13 && g_gipa) {
            VkPhysicalDeviceShaderMixedFloatDotProductFeaturesVALVE vf{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_MIXED_FLOAT_DOT_PRODUCT_FEATURES_VALVE};
            VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &vf};
            auto q = reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures2>(g_gipa(g_instance, "vkGetPhysicalDeviceFeatures2"));
            if (q) { q(pd, &f2); valve_ok = vf.shaderMixedFloatDotProductFloat16AccFloat32 == VK_TRUE; }
            std::vector<const char*> exts(ci->ppEnabledExtensionNames, ci->ppEnabledExtensionNames + ci->enabledExtensionCount);
            VkPhysicalDeviceShaderMixedFloatDotProductFeaturesVALVE enable{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_MIXED_FLOAT_DOT_PRODUCT_FEATURES_VALVE};
            VkDeviceCreateInfo ci2 = *ci;
            if (valve_ok) {
                exts.push_back(VK_VALVE_SHADER_MIXED_FLOAT_DOT_PRODUCT_EXTENSION_NAME);
                enable.shaderMixedFloatDotProductFloat16AccFloat32 = VK_TRUE;
                enable.pNext = const_cast<void*>(ci->pNext);
                ci2.pNext = &enable;
                ci2.enabledExtensionCount = uint32_t(exts.size());
                ci2.ppEnabledExtensionNames = exts.data();
            }
            r = create(pd, &ci2, a, out);
            if (r == VK_SUCCESS) { g_fsr4_features = true; g_fsr4_valve = valve_ok; g_fsr4_wave = wave_ok; Ev("FSR4: device created with shaderInt8+shaderIntegerDotProduct%s%s", valve_ok ? "+VALVE mixed dot" : " (no VALVE ext)", wave_ok ? "+subgroupSizeControl" : ""); }
            else { Ev("FSR4: device creation with extra features FAILED (%d); retrying unmodified", int(r)); chain->u.pLayerInfo = saved_link->pNext; edits.Restore(); }
        } else Ev("FSR4: game feature chain lacks Vulkan1.2/1.3 structs; leaving device unmodified");
    }
    if (r != VK_SUCCESS) { edits.Restore(); r = create(pd, ci, a, out); }
    if (r != VK_SUCCESS) return r;
    auto* dev = new Dev();
    dev->gdpa = gdpa;
#define X(n) dev->n = reinterpret_cast<PFN_vk##n>(gdpa(*out, "vk" #n));
    DEV_FUNCS(X)
#undef X
    if (!dev->CmdPushDescriptorSetKHR) dev->CmdPushDescriptorSetKHR = reinterpret_cast<PFN_vkCmdPushDescriptorSetKHR>(gdpa(*out, "vkCmdPushDescriptorSet"));
    if (!dev->CmdCopyBuffer2) dev->CmdCopyBuffer2 = reinterpret_cast<PFN_vkCmdCopyBuffer2>(gdpa(*out, "vkCmdCopyBuffer2KHR"));
    if (!dev->MapMemory2) dev->MapMemory2 = reinterpret_cast<PFN_vkMapMemory2>(gdpa(*out, "vkMapMemory2KHR"));
    if (!dev->QueueSubmit2) dev->QueueSubmit2 = reinterpret_cast<PFN_vkQueueSubmit2>(gdpa(*out, "vkQueueSubmit2KHR"));
    if (!dev->CmdPipelineBarrier2) dev->CmdPipelineBarrier2 = reinterpret_cast<PFN_vkCmdPipelineBarrier2>(gdpa(*out, "vkCmdPipelineBarrier2KHR"));
    if (!dev->CmdBeginRendering) dev->CmdBeginRendering = reinterpret_cast<PFN_vkCmdBeginRendering>(gdpa(*out, "vkCmdBeginRenderingKHR"));
    { std::lock_guard<std::mutex> l(g_mutex); g_devs[Key(*out)] = dev; }
    g_devs_gen.fetch_add(1, std::memory_order_release);
    g_device = *out; g_pd = pd;
    std::string ex; for (uint32_t i = 0; i < ci->enabledExtensionCount; ++i) ex += std::string(ci->ppEnabledExtensionNames[i]) + " ";
    Ev("DEVICE created; enabled extensions: %s", ex.c_str());
    for (auto* p = reinterpret_cast<const VkBaseInStructure*>(ci->pNext); p; p = p->pNext) {
        if (p->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2) { auto* f = reinterpret_cast<const VkPhysicalDeviceFeatures2*>(p); Ev("FEATURES core: shaderInt16=%u shaderInt64=%u storageImageWriteWithoutFormat=%u storageImageReadWithoutFormat=%u robustBufferAccess=%u shaderStorageImageExtendedFormats=%u", f->features.shaderInt16, f->features.shaderInt64, f->features.shaderStorageImageWriteWithoutFormat, f->features.shaderStorageImageReadWithoutFormat, f->features.robustBufferAccess, f->features.shaderStorageImageExtendedFormats); }
        else if (p->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES) { auto* f = reinterpret_cast<const VkPhysicalDeviceVulkan11Features*>(p); Ev("FEATURES vk1.1: storageBuffer16BitAccess=%u uniformAndStorageBuffer16BitAccess=%u shaderDrawParameters=%u", f->storageBuffer16BitAccess, f->uniformAndStorageBuffer16BitAccess, f->shaderDrawParameters); }
        else if (p->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES) { auto* f = reinterpret_cast<const VkPhysicalDeviceVulkan12Features*>(p); Ev("FEATURES vk1.2: shaderInt8=%u shaderFloat16=%u storageBuffer8BitAccess=%u uniformAndStorageBuffer8BitAccess=%u descriptorIndexing=%u bufferDeviceAddress=%u timelineSemaphore=%u scalarBlockLayout=%u", f->shaderInt8, f->shaderFloat16, f->storageBuffer8BitAccess, f->uniformAndStorageBuffer8BitAccess, f->descriptorIndexing, f->bufferDeviceAddress, f->timelineSemaphore, f->scalarBlockLayout); }
        else if (p->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES) { auto* f = reinterpret_cast<const VkPhysicalDeviceVulkan13Features*>(p); Ev("FEATURES vk1.3: shaderIntegerDotProduct=%u synchronization2=%u dynamicRendering=%u subgroupSizeControl=%u computeFullSubgroups=%u maintenance4=%u", f->shaderIntegerDotProduct, f->synchronization2, f->dynamicRendering, f->subgroupSizeControl, f->computeFullSubgroups, f->maintenance4); }
        else if (p->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COMPUTE_SHADER_DERIVATIVES_FEATURES_KHR) { auto* f = reinterpret_cast<const VkPhysicalDeviceComputeShaderDerivativesFeaturesKHR*>(p); Ev("FEATURES compute_shader_derivatives: linear=%u quads=%u", f->computeDerivativeGroupLinear, f->computeDerivativeGroupQuads); }
        else Ev("FEATURES other pNext sType=%d", int(p->sType));
    }
    if (ci->pEnabledFeatures) Ev("FEATURES pEnabledFeatures: shaderInt16=%u shaderStorageImageWriteWithoutFormat=%u", ci->pEnabledFeatures->shaderInt16, ci->pEnabledFeatures->shaderStorageImageWriteWithoutFormat);
    return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL L_CreateInstance(const VkInstanceCreateInfo* ci, const VkAllocationCallbacks* a, VkInstance* out);
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL L_GetInstanceProcAddr(VkInstance i, const char* name) {
    if (!strcmp(name, "vkCreateInstance")) return (PFN_vkVoidFunction)L_CreateInstance;
    if (!i) return nullptr;
    if (!strcmp(name, "vkCreateDevice")) return (PFN_vkVoidFunction)L_CreateDevice;
    if (!strcmp(name, "vkGetDeviceProcAddr")) return (PFN_vkVoidFunction)L_GetDeviceProcAddr;
    if (!strcmp(name, "vkGetInstanceProcAddr")) return (PFN_vkVoidFunction)L_GetInstanceProcAddr;
    Inst* in = nullptr;
    { std::lock_guard<std::mutex> l(g_mutex); auto it = g_insts.find(Key(i)); if (it != g_insts.end()) in = it->second; }
    if (!in) return nullptr;
    PFN_vkVoidFunction real = in->gipa(i, name);
    // Device-level hooks are also reachable through the instance in some loaders' trampolines.
    if (real) if (auto h = FindHook(name)) return h;
    return real;
}
VKAPI_ATTR VkResult VKAPI_CALL L_CreateInstance(const VkInstanceCreateInfo* ci, const VkAllocationCallbacks* a, VkInstance* out) {
    auto* chain = const_cast<VkLayerInstanceCreateInfo*>(reinterpret_cast<const VkLayerInstanceCreateInfo*>(ci->pNext));
    while (chain && !(chain->sType == VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO && chain->function == VK_LAYER_LINK_INFO)) chain = const_cast<VkLayerInstanceCreateInfo*>(reinterpret_cast<const VkLayerInstanceCreateInfo*>(chain->pNext));
    if (!chain) return VK_ERROR_INITIALIZATION_FAILED;
    PFN_vkGetInstanceProcAddr gipa = chain->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    chain->u.pLayerInfo = chain->u.pLayerInfo->pNext;
    auto create = reinterpret_cast<PFN_vkCreateInstance>(gipa(VK_NULL_HANDLE, "vkCreateInstance"));
    VkResult r = create(ci, a, out);
    if (r != VK_SUCCESS) return r;
    auto* in = new Inst(); in->gipa = gipa; g_gipa = gipa; g_instance = *out;
    { std::lock_guard<std::mutex> l(g_mutex); g_insts[Key(*out)] = in; }

    if (g_inert) return VK_SUCCESS;  // BNG_FSR4 set for some other program: no files, no hooks
    const char* o = getenv("FSR4STUDY_OUT");
    g_out = o ? o : (std::string(getenv("HOME") ? getenv("HOME") : ".") + "/fsr4-native/study/layer-out");
    mkdir(g_out.c_str(), 0755);
    if (!g_lean) mkdir((g_out + "/spv").c_str(), 0755);
    if (const char* f = getenv("FSR4STUDY_CAPTURE_FRAME")) g_capture_start = strtoull(f, nullptr, 10);
    if (const char* c = getenv("FSR4STUDY_CAPTURE_COUNT")) g_capture_count = atoi(c);
    g_events = fopen((g_out + "/events.log").c_str(), "w");
    if (!g_lean) g_frames = fopen((g_out + "/frames.log").c_str(), "w");
    char exe[512] = {}; ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1); (void)n;
    std::string ext; for (uint32_t i = 0; i < ci->enabledExtensionCount; ++i) ext += std::string(ci->ppEnabledExtensionNames[i]) + " ";
    Ev("INSTANCE created in %s pid=%d apiVersion=%u instance extensions: %s", exe, int(getpid()), ci->pApplicationInfo ? ci->pApplicationInfo->apiVersion : 0, ext.c_str());
    return VK_SUCCESS;
}
}  // namespace

extern "C" {
VKAPI_ATTR VkResult VKAPI_CALL vkNegotiateLoaderLayerInterfaceVersion(VkNegotiateLayerInterface* v) {
    if (v->loaderLayerInterfaceVersion >= 2) {
        v->sType = LAYER_NEGOTIATE_INTERFACE_STRUCT;
        v->pNext = nullptr;
        v->pfnGetInstanceProcAddr = L_GetInstanceProcAddr;
        v->pfnGetDeviceProcAddr = L_GetDeviceProcAddr;
        v->pfnGetPhysicalDeviceProcAddr = nullptr;
    }
    if (v->loaderLayerInterfaceVersion > 2) v->loaderLayerInterfaceVersion = 2;
    return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL vkCreateInstance(const VkInstanceCreateInfo* ci, const VkAllocationCallbacks* a, VkInstance* i) { return L_CreateInstance(ci, a, i); }
}

// The backend (compiled with -DvkGetPhysicalDevice...=f4_vkGetPhysicalDevice...) must not hand the layer-level physical
// device to the loader's trampolines (they only accept the application-visible handle), so these two queries go down the chain.
extern "C" {
void f4_vkGetPhysicalDeviceMemoryProperties(VkPhysicalDevice pd, VkPhysicalDeviceMemoryProperties* p) {
    reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(g_gipa(g_instance, "vkGetPhysicalDeviceMemoryProperties"))(pd, p);
    static bool logged = false;
    if (!logged) { logged = true; for (uint32_t i = 0; i < p->memoryTypeCount; ++i) Ev("FSR4: backend memory type %u flags=%x heap=%u", i, p->memoryTypes[i].propertyFlags, p->memoryTypes[i].heapIndex); }
}
void f4_vkGetPhysicalDeviceProperties(VkPhysicalDevice pd, VkPhysicalDeviceProperties* p) {
    reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(g_gipa(g_instance, "vkGetPhysicalDeviceProperties"))(pd, p);
    Ev("FSR4: backend GPU %s UBO alignment=%llu maxStorageBufferRange=%u", p->deviceName, (unsigned long long)p->limits.minUniformBufferOffsetAlignment, p->limits.maxStorageBufferRange);
}
}

// Backend-only probes: keep upstream untouched and forward below this layer.
extern "C" VkResult f4_vkCreateBuffer(VkDevice d, const VkBufferCreateInfo* ci, const VkAllocationCallbacks* a, VkBuffer* b) {
    auto copy = *ci;
    if (getenv("FSR4STUDY_WEIGHTS") || getenv("FSR4STUDY_UNIFORMS")) copy.usage |= VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    return L_CreateBuffer(d, &copy, a, b);
}
extern "C" void f4_vkCmdCopyBuffer(VkCommandBuffer c, VkBuffer src, VkBuffer dst, uint32_t n, const VkBufferCopy* regions) {
    L_CmdCopyBuffer(c, src, dst, n, regions);
    if (!getenv("FSR4STUDY_WEIGHTS") || g_weight_rb || n != 1 || regions[0].size != 131072) return;
    auto* dev = D(c);
    BufInfo sb; char* mapped = nullptr;
    { std::lock_guard<std::mutex> lock(g_mutex); sb = g_bufs[(uint64_t)src]; auto it = g_mapped.find(sb.mem); if (it != g_mapped.end()) mapped = it->second; }
    if (!mapped) { Ev("FSR4: weights source is not in mapped shadow"); return; }
    g_weight_expected.assign((uint8_t*)mapped + sb.off + regions[0].srcOffset, (uint8_t*)mapped + sb.off + regions[0].srcOffset + regions[0].size);
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO}; bi.size = regions[0].size; bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    if (dev->CreateBuffer(g_device, &bi, nullptr, &g_weight_rb) != VK_SUCCESS) return;
    VkMemoryRequirements req; dev->GetBufferMemoryRequirements(g_device, g_weight_rb, &req);
    VkPhysicalDeviceMemoryProperties mp; f4_vkGetPhysicalDeviceMemoryProperties(g_pd, &mp);
    uint32_t ti = UINT32_MAX; for (uint32_t i=0; i<mp.memoryTypeCount; ++i) if ((req.memoryTypeBits & (1u<<i)) && (mp.memoryTypes[i].propertyFlags & (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) == (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) { ti=i; break; }
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; ai.allocationSize=req.size; ai.memoryTypeIndex=ti; VkDeviceMemory mem;
    if (ti == UINT32_MAX || dev->AllocateMemory(g_device, &ai, nullptr, &mem) != VK_SUCCESS || dev->BindBufferMemory(g_device, g_weight_rb, mem, 0) != VK_SUCCESS) return;
    void* ptr; if (dev->MapMemory(g_device, mem, 0, VK_WHOLE_SIZE, 0, &ptr) != VK_SUCCESS) return;
    g_weight_ptr = (uint8_t*)ptr; memset(ptr, 0xee, bi.size);
    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER}; mb.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT; mb.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
    dev->CmdPipelineBarrier(c, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
    VkBufferCopy r{regions[0].dstOffset,0,regions[0].size}; dev->CmdCopyBuffer(c,dst,g_weight_rb,1,&r);
    mb.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT; mb.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
    dev->CmdPipelineBarrier(c,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&mb,0,nullptr,0,nullptr);
    g_weight_record_counter = g_f4.counter;
    Ev("FSR4: weight upload probe recorded bytes=%llu sourceMemTypeReadback=%u", (unsigned long long)bi.size, ti);
}
