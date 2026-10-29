# PaperDollTouch — Minecraft Bedrock 26.60 (Android ARM64)

Native mod untuk **LeviLauncher Android** (`preload-native`, `arm64-v8a`) yang memungkinkan pemain memutar preview karakter (paper doll) di layar inventory menggunakan sentuhan dan geseran jari (horizontal yaw & vertical pitch) tanpa mengubah arah pandang pemain di dunia.

## Arsitektur Proyek

Proyek ini dibangun di atas fondasi `levilauncher-android-mod-template` yang telah terbukti berhasil dikompilasi di GitHub Actions (`ndk;28.2.13676358`, CMake + Ninja + `scripts/package.ps1`), dengan seluruh contoh `clange_me` digantikan oleh implementasi C++20 `paperdoll::PaperDollTouchMod`:

- `CMakeLists.txt` — Mengonfigurasi target `PaperDollTouch` (`libPaperDollTouch.so`) dan paket `PaperDollTouch.levipack`.
- `src/main.cpp` — Registrasi entry point `PL_REGISTER_MOD(paperdoll::PaperDollTouchMod, ...)`.
- `src/mod/Config.h` & `src/mod/Config.cpp` — Skema konfigurasi terketik (`pl::config::Schema<paperdoll::ModConfig>`).
- `src/mod/PaperDollTouchMod.h` & `src/mod/PaperDollTouchMod.cpp` — State machine input sentuh lock-free (`std::atomic`), pemindai simbol ELF64 `.dynsym` satu-kali (`dl_iterate_phdr`), dan detour `HudPlayerRenderer::render` dengan *Scoped Save-and-Restore* orientasi karakter.

Baca `BUILD_INSTRUCTIONS.txt` untuk panduan lengkap kompilasi di GitHub Actions dan instalasi di LeviLauncher langsung dari HP Android.
