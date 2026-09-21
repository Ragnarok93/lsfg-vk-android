// Compiles the production Android dispatch implementation against fake drivers.
// No GPU is required; all handles and downstream PFNs are controlled by this test.
#include "../src/layer_android.cpp"
#include "../src/mini/semaphore.cpp"
#include <cassert>
#include <vector>
#include <array>
#include <set>
#include <algorithm>

namespace LSFG {
vulkan_error::vulkan_error(VkResult r, const std::string& m) : std::runtime_error(m), result(r) {}
vulkan_error::~vulkan_error() noexcept = default;
}
namespace Config { Configuration snapshot() { return {}; } }
namespace Hooks { std::unordered_map<std::string, PFN_vkVoidFunction> hooks; }
namespace Utils { void logLimitN(const char*, int, const std::string&) {} }

struct SwapchainState { int tag; };
struct SwapchainKey {
    VkDevice device; VkSwapchainKHR swapchain;
    bool operator==(const SwapchainKey&) const = default;
};
struct SwapchainKeyHash {
    size_t operator()(const SwapchainKey& k) const {
        return std::hash<VkDevice>{}(k.device) ^ std::hash<VkSwapchainKHR>{}(k.swapchain);
    }
};
std::mutex hookStateMutex;
std::unordered_map<SwapchainKey, std::shared_ptr<SwapchainState>, SwapchainKeyHash> swapchains;
// Generated from the production hooks/context methods by the runner.
#include "production_lifetime_methods.inc"

struct FakeDispatchable { void* dispatch; };
static int loaderKeyAStorage, loaderKeyBStorage;
static FakeDispatchable d1Storage{&loaderKeyAStorage};
static FakeDispatchable d2Storage{&loaderKeyBStorage};
static FakeDispatchable d3Storage{&loaderKeyAStorage};
static FakeDispatchable d4Storage{&loaderKeyBStorage};
static VkDevice d1 = reinterpret_cast<VkDevice>(&d1Storage);
static VkDevice d2 = reinterpret_cast<VkDevice>(&d2Storage);
static VkDevice d3 = reinterpret_cast<VkDevice>(&d3Storage);
static VkDevice d4 = reinterpret_cast<VkDevice>(&d4Storage);
static VkQueue q1 = reinterpret_cast<VkQueue>(0x3000);
static VkQueue q2 = reinterpret_cast<VkQueue>(0x4000);
static VkQueue q3 = reinterpret_cast<VkQueue>(0x5000);
static int submitted1, submitted2, submitted3, presented1, presented2;
static std::set<VkSemaphore> alive;
static uintptr_t nextSemaphore = 100;
static VKAPI_ATTR void VKAPI_CALL getQueue(VkDevice d, uint32_t, uint32_t, VkQueue* q) {
    *q = d == d1 ? q1 : (d == d2 ? q2 : q3);
}
static VKAPI_ATTR void VKAPI_CALL getQueue2(VkDevice d, const VkDeviceQueueInfo2*, VkQueue* q) { getQueue(d, 0, 0, q); }
static VKAPI_ATTR VkResult VKAPI_CALL submit1(VkQueue q, uint32_t, const VkSubmitInfo*, VkFence) { assert(q == q1); ++submitted1; return VK_SUCCESS; }
static VKAPI_ATTR VkResult VKAPI_CALL submit2(VkQueue q, uint32_t, const VkSubmitInfo*, VkFence) { assert(q == q2); ++submitted2; return VK_SUCCESS; }
static VKAPI_ATTR VkResult VKAPI_CALL submit3(VkQueue q, uint32_t, const VkSubmitInfo*, VkFence) { assert(q == q3); ++submitted3; return VK_SUCCESS; }
static VKAPI_ATTR VkResult VKAPI_CALL present1(VkQueue q, const VkPresentInfoKHR*) { assert(q == q1); ++presented1; return VK_SUCCESS; }
static VKAPI_ATTR VkResult VKAPI_CALL present2(VkQueue q, const VkPresentInfoKHR*) { assert(q == q2); ++presented2; return VK_SUCCESS; }
static PFN_vkVoidFunction constructionGdpa(VkDevice d, const char* name) {
    if (std::strcmp(name, "vkGetDeviceQueue") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(getQueue);
    if (std::strcmp(name, "vkGetDeviceQueue2") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(getQueue2);
    if (std::strcmp(name, "vkQueueSubmit") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(d == d1 ? submit1 : submit2);
    if (std::strcmp(name, "vkQueuePresentKHR") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(d == d1 ? present1 : present2);
    return nullptr;
}
static PFN_vkVoidFunction constructionGdpaNoQueue(VkDevice d, const char* name) {
    if (std::strcmp(name, "vkQueueSubmit") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(d == d3 ? submit3 : submit2);
    return nullptr;
}
static VKAPI_ATTR VkResult VKAPI_CALL createSem(VkDevice, const VkSemaphoreCreateInfo*, const VkAllocationCallbacks*, VkSemaphore* s) {
    *s = reinterpret_cast<VkSemaphore>(nextSemaphore++); alive.insert(*s); return VK_SUCCESS;
}
static VKAPI_ATTR void VKAPI_CALL destroySem(VkDevice d, VkSemaphore s, const VkAllocationCallbacks*) { assert(d == d1); assert(alive.erase(s) == 1); }

int main() {
    // Regression: Turnip/wrapper-gamenative may request queues from inside the
    // downstream vkCreateDevice call.  The full device dispatch table does not
    // exist yet, but the construction thread's exact downstream GDPA does.
    {
        DeviceConstructionScope construction(constructionGdpa);
        VkQueue constructionQ1{}, constructionQ2{};
        Layer::ovkGetDeviceQueue(d1, 0, 0, &constructionQ1);
        VkDeviceQueueInfo2 constructionInfo{VK_STRUCTURE_TYPE_DEVICE_QUEUE_INFO_2};
        Layer::ovkGetDeviceQueue2(d2, &constructionInfo, &constructionQ2);
        assert(constructionQ1 == q1 && constructionQ2 == q2);
        assert(Layer::queueOwner(q1) == d1 && Layer::queueOwner(q2) == d2);
        assert(Layer::ovkQueueSubmit(q1, 0, nullptr, {}) == VK_SUCCESS);
        assert(Layer::ovkQueueSubmit(q2, 0, nullptr, {}) == VK_SUCCESS);
        assert(Layer::ovkQueuePresentKHR(q1, nullptr) == VK_SUCCESS);
        assert(Layer::ovkQueuePresentKHR(q2, nullptr) == VK_SUCCESS);
    }
    eraseDeviceDispatch(d1);
    eraseDeviceDispatch(d2);
    submitted1 = submitted2 = presented1 = presented2 = 0;

    // Regression: Turnip/wrapper-gamenative can expose the new VkDevice handle
    // before vkGetDeviceProcAddr(newDevice, "vkGetDeviceQueue") is usable.
    // The known-good S20+ path reused the established queue thunk for devices
    // sharing the same loader dispatch key. Preserve that bootstrap behavior
    // only during construction; exact ownership must take over afterwards.
    DeviceDispatch bootstrap{};
    bootstrap.device = d1;
    bootstrap.GetDeviceQueue = getQueue;
    bootstrap.GetDeviceQueue2 = getQueue2;
    bootstrap.QueueSubmit = submit1;
    storeDeviceDispatch(d1, bootstrap);
    {
        DeviceConstructionScope construction(constructionGdpaNoQueue);
        VkQueue constructionQ3{};
        Layer::ovkGetDeviceQueue(d3, 0, 0, &constructionQ3);
        assert(constructionQ3 == q3);
        assert(Layer::queueOwner(q3) == d3);

        VkQueue mismatched{};
        Layer::ovkGetDeviceQueue(d4, 0, 0, &mismatched);
        assert(mismatched == VK_NULL_HANDLE);
    }
    // Private framegen re-entry can also reach our queue wrapper without a
    // DeviceConstructionScope at all (the private instance deliberately bypasses
    // the game-instance compatibility globals). Same-key bootstrap must still work.
    eraseDeviceDispatch(d3);
    {
        VkQueue privateQ{};
        Layer::ovkGetDeviceQueue(d3, 0, 0, &privateQ);
        assert(privateQ == q3);
        assert(Layer::queueOwner(q3) == d3);
    }

    DeviceDispatch completed = bootstrap;
    completed.device = d3;
    completed.QueueSubmit = submit3;
    storeDeviceDispatch(d3, completed);
    assert(Layer::ovkQueueSubmit(q3, 0, nullptr, {}) == VK_SUCCESS);
    eraseDeviceDispatch(d1);
    eraseDeviceDispatch(d3);
    submitted1 = submitted2 = submitted3 = presented1 = presented2 = 0;

    DeviceDispatch a{}, b{};
    a.device = d1; a.GetDeviceQueue = getQueue; a.GetDeviceQueue2 = getQueue2;
    a.QueueSubmit = submit1; a.QueuePresentKHR = present1;
    a.CreateSemaphore = createSem; a.DestroySemaphore = destroySem;
    b = a; b.device = d2; b.QueueSubmit = submit2; b.QueuePresentKHR = present2;
    storeDeviceDispatch(d1, a); storeDeviceDispatch(d2, b);
    VkQueue qa{}, qb{};
    Layer::ovkGetDeviceQueue(d1, 0, 0, &qa);
    VkDeviceQueueInfo2 qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_INFO_2};
    Layer::ovkGetDeviceQueue2(d2, &qi, &qb);
    assert(qa == q1 && qb == q2);
    assert(Layer::queueOwner(q1) == d1 && Layer::queueOwner(q2) == d2);
    assert(layer_vkGetDeviceProcAddr(d2, "vkGetDeviceQueue2") == reinterpret_cast<PFN_vkVoidFunction>(Layer::ovkGetDeviceQueue2));
    b.GetDeviceQueue2 = nullptr; storeDeviceDispatch(d2, b);
    assert(layer_vkGetDeviceProcAddr(d2, "vkGetDeviceQueue2") == nullptr);
    for (int n = 0; n < 32; ++n) {
        assert(Layer::ovkQueueSubmit(q1, 0, nullptr, {}) == VK_SUCCESS);
        assert(Layer::ovkQueueSubmit(q2, 0, nullptr, {}) == VK_SUCCESS);
        assert(Layer::ovkQueuePresentKHR(q1, nullptr) == VK_SUCCESS);
        assert(Layer::ovkQueuePresentKHR(q2, nullptr) == VK_SUCCESS);
    }
    assert(submitted1 == 32 && submitted2 == 32 && presented1 == 32 && presented2 == 32);
    VkQueue unknown = reinterpret_cast<VkQueue>(0xdead);
    assert(Layer::ovkQueueSubmit(unknown, 0, nullptr, {}) == VK_ERROR_DEVICE_LOST);
    assert(Layer::ovkQueuePresentKHR(unknown, nullptr) == VK_ERROR_DEVICE_LOST);
    const auto sc = reinterpret_cast<VkSwapchainKHR>(0x42);
    auto s1 = std::make_shared<SwapchainState>(SwapchainState{1});
    auto s2 = std::make_shared<SwapchainState>(SwapchainState{2});
    swapchains[{d1, sc}] = s1; swapchains[{d2, sc}] = s2;
    assert(findSwapchainState(sc, q1) == s1 && findSwapchainState(sc, q2) == s2);
    assert(!findSwapchainState(sc, unknown));
    swapchains.erase({d1, sc});
    assert(!findSwapchainState(sc, q1)); // unique other-device handle is never a fallback
    eraseDeviceDispatch(d2);
    assert(Layer::queueOwner(q2) == VK_NULL_HANDLE);
    assert(Layer::ovkQueuePresentKHR(q2, nullptr) == VK_ERROR_DEVICE_LOST);
    // Reused queue handle becomes valid only through a new exact acquisition.
    a.device = d1; storeQueueDispatch(q2, a);
    assert(findSwapchainState(sc, q2) == nullptr);
    storeQueueDispatch(q2, b); // live ownership collision poisons the entry
    assert(Layer::queueOwner(q2) == VK_NULL_HANDLE);
    std::cout << "PASS construction-time queue acquisition, queue1/queue2, two-device submits/presents, unknown owner, colliding/reused swapchains, device cleanup\n";

    LsContext ctx;
    ctx.wsiConsumersByImage_.resize(32);
    std::array<LsContext::RenderPassInfo, 8> ring;
    for (uint64_t generation = 1; generation <= 32; ++generation) {
        auto& producer = ring[generation % 8];
        producer.queueConsumerSemaphores.clear(); // completed producer, wraps four times
        producer.generation = generation;
        Mini::Semaphore sem(d1);
        ctx.retainWsiConsumersForImage(generation - 1, generation, {sem}, "test");
    }
    assert(alive.size() == 32); // no destructor on producer completion or ring wrap
    // Returning an acquired image is not completion: the owner moves into the
    // real acquire-wait submission's pass, and stays alive until its fence.
    for (uint32_t image = 0; image < 32; ++image) {
        LsContext::RenderPassInfo consumer; consumer.generation = image + 33;
        ctx.transferWsiConsumersToPass(image, consumer, "delayed-acquisition");
        assert(alive.size() == 32 - image);
        consumer.queueConsumerSemaphores.clear(); // consuming fence observed complete
        assert(alive.size() == 31 - image);
    }
    { Mini::Semaphore first(d1), second(d1);
      ctx.retainWsiConsumersForImage(0, 65, {first}, "source-only");
      ctx.retainWsiConsumersForImage(0, 66, {second}, "source-only"); }
    assert(alive.size() == 2); // append unresolved owners, never replace
    LsContext::RenderPassInfo last;
    ctx.transferWsiConsumersToPass(0, last, "resume");
    assert(alive.size() == 2);
    last.queueConsumerSemaphores.clear();
    assert(alive.empty());
    std::cout << "PASS 32 delayed WSI generations, four ring wraps, acquire-return delay, semaphore destructor ordering, source-only retention\n";
}
