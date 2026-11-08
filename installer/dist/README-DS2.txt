Mod Engine 2 - Dark Souls II: Scholar of the First Sin
=====================================================

这是 Mod Engine 2 的 Dark Souls II: SotFS（Steam，AppID 335300）支持版本。
核心功能与 DS3/Elden Ring 版本一致：把 "mod" 目录里的文件覆盖到游戏的
gamedata/dlc_data 虚拟路径上，不需要解包或重打包 .bdt/.bhd。

安装
----
1. 把本目录内的所有文件解压到游戏目录（DarkSoulsII.exe 所在目录），例如：
   D:\SteamLibrary\steamapps\common\Dark Souls II Scholar of the First Sin\Game
   解压后应包含：
     modengine2_launcher.exe
     modengine2\bin\modengine2.dll
     modengine2\bin\lua.dll
     config_darksouls2.toml
     launchmod_darksouls2.bat
     steam_appid.txt
2. 保持 Steam 已启动并已登录。
3. 双击 launchmod_darksouls2.bat（或运行：
   modengine2_launcher.exe -t ds2 -c config_darksouls2.toml）

注意：不要删除游戏目录里的 steam_appid.txt。DS2 的 exe 带 SteamStub，
没有这个文件时游戏会在启动几秒后退出。launcher 在缺失时会自动补写。

mod 目录
--------
默认读取游戏目录下的 "mod" 文件夹，路径按 gamedata 虚拟路径展开，例如：
  mod\menu\Text\Chinese\common.fmg   ->  gamedata:/menu/Text/Chinese/common.fmg
  mod\param\ItemParam.param          ->  gamedata:/param/ItemParam.param
  mod\font\Chinese\FeFont_Big.fontbnd.dcx

支持多个 mod：编辑 config_darksouls2.toml 的 mods 列表，path 相对于 config 文件：

  [extension.mod_loader]
  enabled = true
  mods = [
      { enabled = true, name = "default", path = "mod" },
      { enabled = false, name = "other", path = "mods\\other" }
  ]

日志与排错
----------
日志：modengine2\logs\modengine_YYYY-MM-DD.log
把 config 里的 [modengine] debug 改为 true 会输出详细路径匹配日志，并打开控制台。

已知限制
--------
- 只覆盖经过游戏 VirtualToArchivePath 的文件路径；直接由其它系统读取的文件不在此列。
- 在线模式使用 mod 有被 softban 的风险，请自行判断。
- 本版本未启用 crashpad 崩溃上报（构建时关闭）。

构建
----
需要 Visual Studio（MSVC x64）、CMake 3.17+、vcpkg 以及仓库子模块：
  vcpkg install detours spdlog tomlplusplus lua sol2 winreg cli11 nano-signal-slot --triplet x64-windows
  cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release ^
        -DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake ^
        -DVCPKG_TARGET_TRIPLET=x64-windows -DMODENGINE_ENABLE_CRASHPAD=OFF
  cmake --build build

实现说明
--------
ME2 在 DS2 上注册了两类 hook：
  1. kernel32!CreateFileW：把游戏目录相对路径重定向到 mod 根目录。
  2. DS2 的 VirtualToArchivePath（AOB: 41 54 41 56 41 57 48 83 EC 40 ...）：
     在调用原函数前，把 gamedata:/... / dlc_data:/... 前缀原地改写为 ./...
     当且仅当 mod 目录存在同名文件。
两段式配合即可让游戏从磁盘加载 mod 文件，而不触碰归档。
