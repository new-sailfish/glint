#pragma once

namespace glint {
int run_desktop();
int run_native_host();
int run_register(bool unregister_host, const wchar_t* chrome_id, const wchar_t* edge_id);
}
