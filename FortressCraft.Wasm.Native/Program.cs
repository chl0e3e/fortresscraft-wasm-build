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

// Console output is mirrored onto the on-page boot overlay by main.js, so these stages double as
// live boot progress AND as the diagnostic when the screen would otherwise just go black.
Console.WriteLine($"loader: booting {asmName}!{typeName} (SDL3 + OffscreenCanvas)");

try
{
    // The injected game assembly is in the boot manifest, so resolve it by simple name.
    Console.WriteLine($"loader: resolving assembly '{asmName}'");
    Assembly gameAsm = Assembly.Load(new AssemblyName(asmName));

    Console.WriteLine($"loader: resolving entry type '{typeName}'");
    Type gameType = gameAsm.GetType(typeName, throwOnError: true);

    // FortressCraft's Game1 derives from Microsoft.Xna.Framework.Game; construct + Run.
    Console.WriteLine("loader: constructing game (Game1 ctor: SteamManager, content, world)");
    using var game = (Game)Activator.CreateInstance(gameType);

    Console.WriteLine("loader: game.Run() — entering FNA main loop");
    game.Run();
}
catch (Exception ex)
{
    // Surface the full exception (incl. fail-closed Steam/ownership refusals) instead of dying
    // silently to a black screen. main.js shows this on the overlay and POSTs it to /__log.
    Console.WriteLine("loader: FATAL during boot:");
    for (Exception e = ex; e != null; e = e.InnerException)
        Console.WriteLine($"  {e.GetType().Name}: {e.Message}");
    Console.WriteLine(ex.StackTrace);
    throw;
}
