#include <reshade.hpp>

#include <Windows.h>

#include <cstdint>
#include <iostream>

namespace
{
uint32_t g_addons = 0;
uint32_t g_events = 0;
uint32_t g_unregistered_events = 0;
}

extern "C" __declspec(dllexport) bool ReShadeRegisterAddon(void *, uint32_t api_version)
{
	if (api_version != RESHADE_API_VERSION)
		return false;
	++g_addons;
	return true;
}

extern "C" __declspec(dllexport) void ReShadeUnregisterAddon(void *)
{
	--g_addons;
}

extern "C" __declspec(dllexport) void ReShadeRegisterEvent(reshade::addon_event, void *)
{
	++g_events;
}

extern "C" __declspec(dllexport) void ReShadeUnregisterEvent(reshade::addon_event, void *)
{
	++g_unregistered_events;
}

int wmain(int argc, wchar_t **argv)
{
	if (argc != 2)
		return 2;

	HMODULE addon = LoadLibraryW(argv[1]);
	if (addon == nullptr)
	{
		std::cerr << "LoadLibrary failed: " << GetLastError() << '\n';
		return 1;
	}
	if (g_addons != 1 || g_events != 8 || GetProcAddress(addon, "NAME") == nullptr ||
		GetProcAddress(addon, "DESCRIPTION") == nullptr)
	{
		std::cerr << "Registration or metadata export check failed.\n";
		FreeLibrary(addon);
		return 1;
	}

	FreeLibrary(addon);
	if (g_addons != 0 || g_unregistered_events != 8)
	{
		std::cerr << "Unregistration check failed.\n";
		return 1;
	}
	std::cout << "Loaded, registered 8 callbacks, exported metadata, and unloaded cleanly.\n";
	return 0;
}
