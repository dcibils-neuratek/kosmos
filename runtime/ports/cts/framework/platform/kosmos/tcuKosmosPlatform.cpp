/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * **The CTS's platform on Kosmos** (`docs/gpu3d.md` G2): a `tcu::Platform`
 * with Vulkan and nothing else. There is no Vulkan loader on Kosmos and no
 * library to open: lavapipe is linked into the image, and its
 * `vk_icdGetInstanceProcAddr` is the one function the suite is handed - as
 * `vkGetInstanceProcAddr`, the name a loader would have given it - and
 * every other is asked of it, as the CTS's PlatformDriver asks a loader's.
 * No window system: the tests that want one are not supported here.
 */

#include "tcuPlatform.hpp"
#include "tcuFunctionLibrary.hpp"
#include "vkPlatform.hpp"

#include <ostream>

/* lavapipe's, in plain C types: the CTS has Vulkan's types of its own. */
typedef void (*VoidFunction)(void);
extern "C" VoidFunction vk_icdGetInstanceProcAddr(void *instance, const char *name);

namespace
{

const tcu::StaticFunctionLibrary::Entry lavapipe[] = {
    {"vkGetInstanceProcAddr", (deFunctionPtr)vk_icdGetInstanceProcAddr},
};

class KosmosLibrary : public vk::Library
{
public:
    KosmosLibrary(void) : m_library(lavapipe, DE_LENGTH_OF_ARRAY(lavapipe)), m_driver(m_library)
    {
    }

    const vk::PlatformInterface &getPlatformInterface(void) const
    {
        return m_driver;
    }

    const tcu::FunctionLibrary &getFunctionLibrary(void) const
    {
        return m_library;
    }

private:
    const tcu::StaticFunctionLibrary m_library;
    const vk::PlatformDriver m_driver;
};

class KosmosVulkanPlatform : public vk::Platform
{
public:
    vk::Library *createLibrary(LibraryType libraryType, const char *) const
    {
        if (libraryType != LIBRARY_TYPE_VULKAN)
            TCU_THROW(NotSupportedError, "Kosmos has only Vulkan's library");
        return new KosmosLibrary();
    }

    void describePlatform(std::ostream &dst) const
    {
        dst << "OS: Kosmos\n";
#if defined(__x86_64__)
        dst << "CPU: x86-64\n";
#elif defined(__aarch64__)
        dst << "CPU: AArch64\n";
#endif
    }
};

class KosmosPlatform : public tcu::Platform
{
public:
    const vk::Platform &getVulkanPlatform(void) const
    {
        return m_vulkan;
    }

private:
    KosmosVulkanPlatform m_vulkan;
};

} // namespace

tcu::Platform *createPlatform(void)
{
    return new KosmosPlatform();
}
