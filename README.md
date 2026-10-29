# Minecraft Mobile Cursor — Windows-Style GUI System (PaperDollTouch v2.0.0)

Mod native C++20 (`preload-native`, `arm64-v8a`) untuk **LeviLauncher Android** pada **Minecraft Bedrock 26.60** yang menghadirkan pengalaman **Kursor Minecraft Windows** di perangkat mobile layar sentuh, terintegrasi penuh dengan **PaperDollTouch**.

## Fitur Utama

1. **Kursor Pointer Putih Windows (Hanya Saat GUI Terbuka)**:
   - Aktif otomatis saat membuka Player Inventory, Chest, Crafting Table, Furnace, Anvil, Villager Trading, Pause Menu, dan Settings.
   - Langsung menghilang sepenuhnya saat kembali ke gameplay normal (tidak muncul di HUD dan tidak mengganggu kontrol kamera).
   - Otomatis disembunyikan jika mouse fisik (`AINPUT_SOURCE_MOUSE`) sedang digunakan.
2. **Green Slot Hover (`#38E54D`)**:
   - Menampilkan border hijau ala Minecraft Windows tepat pada slot yang sedang ditunjuk kursor tanpa menutupi ikon item, jumlah stack, durability bar, atau tooltip.
3. **Kepala & Badan Player Mengikuti Kursor (PaperDoll Cursor-Follow)**:
   - Mengintegrasikan perbaikan vtable `3Mob` (slot 14 & 16) + `5Actor::getInterpolatedHeadRot` (slot 15) + `ActorRotationComponent` (TBI-safe) sehingga kepala, leher, dan badan Paper Doll menoleh serta mendongak/menunduk mengikuti kursor.
4. **Menu Pengaturan `Mobile Cursor Settings`**:
   - Klik tombol **`[Cursor]`** di pojok kanan atas saat membuka Inventory/Pause Menu untuk memilih ukuran **Small (`0.75×`)**, **Medium (`1.00×` bawaan)**, **Large (`1.50×`)**, atau menggeser slider **`0.75× – 2.00×`** secara langsung tanpa restart game.
