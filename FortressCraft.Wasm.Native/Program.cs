using System;
using System.Reflection;
using Microsoft.Xna.Framework;

// FortressCraft.Wasm.Native — a GENERIC FNA-on-wasm loader (net10, threads + OffscreenCanvas).
//
// Contains NO game code. The game assembly (FortressCraft.dll + deps), browser-patched from the
// player's own install by SeeOneModLoader and injected into _framework, is loaded + booted here
// by reflection. This is what keeps the build-once server bundle game-agnostic and copyright-clean,
// and makes it reusable for any FNA game.
//
// SDL3 backend (matches our FNA 26.06 + the -pthread SDL3 native build) + FNA3D's OpenGL driver
// -> WebGL2. The OffscreenCanvas is transferred to this worker by the boot JS, so FNA3D creates
// its GL context on the same thread it draws from — fixing the deputy-thread GLctx crash.
Environment.SetEnvironmentVariable("FNA_PLATFORM_BACKEND", "SDL3");
Environment.SetEnvironmentVariable("FNA3D_FORCE_DRIVER", "OpenGL");

// Which injected assembly + entry type to boot. Defaults to FortressCraft; the patcher/loader can
// override via env so the same bundle boots any FNA game.
string asmName  = Environment.GetEnvironmentVariable("FNA_GAME_ASSEMBLY") ?? "FortressCraft";
string typeName = Environment.GetEnvironmentVariable("FNA_GAME_ENTRY_TYPE") ?? "BasicXNAProject.Game1";

Console.WriteLine($"FNA-wasm loader: booting {asmName}!{typeName} (SDL3 + OffscreenCanvas)");

// The injected game assembly is in the boot manifest, so resolve it by simple name.
Assembly gameAsm = Assembly.Load(new AssemblyName(asmName));
Type gameType = gameAsm.GetType(typeName, throwOnError: true);

// FortressCraft's Game1 derives from Microsoft.Xna.Framework.Game; construct + Run.
using var game = (Game)Activator.CreateInstance(gameType);
game.Run();
