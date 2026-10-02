#!/usr/bin/env node
/**
 * tools/offsets/map_offsets.mjs
 *
 * Normalises a theo's-offsets dump (https://offsets.imtheo.lol) into the schema
 * the DLL consumes: lowercase dotted keys, hex values, per-key kind.
 *
 * Why a mapping table instead of using the dump's names verbatim:
 *   - the dumper's namespaces are PascalCase and move; our keys are an internal
 *     ABI (C++ calls off::Get("data_model.script_context")), so they must be
 *     stable across dumps;
 *   - the dump mixes two kinds of value (image-relative pointer vs struct
 *     offset) under the same namespace, and the C++ side must not guess which
 *     is which;
 *   - a renamed/removed namespace must be a hard error, not a silent zero.
 *
 * Rules enforced here:
 *   - decimal in, hex out (the dump is decimal; humans and C++ want hex);
 *   - `0` means "not dumped"  → the key is OMITTED, never emitted as 0;
 *   - an unknown key in --strict mode fails the run.
 *
 * Usage:
 *   node tools/offsets/map_offsets.mjs --in <dump.json> --out <embedded.json> [--strict]
 *   node tools/offsets/map_offsets.mjs --in <dump.json> --stdout
 */

import { readFileSync, writeFileSync } from "node:fs";

export const SCHEMA = "phetamine.offsets/1";

/** internal key  →  { path: dumper path, kind: "rva" | "offset" } */
export const KEYMAP = Object.freeze({
  // ---- scheduler / frame -------------------------------------------------
  "task_scheduler.pointer":        { path: "TaskScheduler.Pointer", kind: "rva" },
  "task_scheduler.job_start":      { path: "TaskScheduler.JobStart", kind: "offset" },
  "task_scheduler.job_end":        { path: "TaskScheduler.JobEnd", kind: "offset" },
  "task_scheduler.job_name":       { path: "TaskScheduler.JobName", kind: "offset" },
  "task_scheduler.max_fps":        { path: "TaskScheduler.MaxFPS", kind: "offset" },
  "run_service.heartbeat_task":    { path: "RunService.HeartbeatTask", kind: "offset" },
  "run_service.heartbeat_fps":     { path: "RunService.HeartbeatFPS", kind: "offset" },
  "render_job.fake_data_model":    { path: "RenderJob.FakeDataModel", kind: "offset" },
  "render_job.real_data_model":    { path: "RenderJob.RealDataModel", kind: "offset" },
  "render_job.render_view":        { path: "RenderJob.RenderView", kind: "offset" },
  "visual_engine.pointer":         { path: "VisualEngine.Pointer", kind: "rva" },
  "visual_engine.fake_data_model": { path: "VisualEngine.FakeDataModel", kind: "offset" },
  "visual_engine.render_view":     { path: "VisualEngine.RenderView", kind: "offset" },
  "visual_engine.view_matrix":     { path: "VisualEngine.ViewMatrix", kind: "offset" },
  "visual_engine.dimensions":      { path: "VisualEngine.Dimensions", kind: "offset" },
  // ---- data model --------------------------------------------------------
  "fake_data_model.pointer":       { path: "FakeDataModel.Pointer", kind: "rva" },
  "fake_data_model.real":          { path: "FakeDataModel.RealDataModel", kind: "offset" },
  "data_model.script_context":     { path: "DataModel.ScriptContext", kind: "offset" },
  "data_model.job_id":             { path: "DataModel.JobId", kind: "offset" },
  "data_model.place_id":           { path: "DataModel.PlaceId", kind: "offset" },
  "data_model.game_id":            { path: "DataModel.GameId", kind: "offset" },
  "data_model.creator_id":         { path: "DataModel.CreatorId", kind: "offset" },
  "data_model.place_version":      { path: "DataModel.PlaceVersion", kind: "offset" },
  "data_model.game_loaded":        { path: "DataModel.GameLoaded", kind: "offset" },
  "data_model.workspace":          { path: "DataModel.Workspace", kind: "offset" },
  "data_model.server_ip":          { path: "DataModel.ServerIP", kind: "offset" },
  "data_model.primitive_count":    { path: "DataModel.PrimitiveCount", kind: "offset" },
  // ---- instance layout ---------------------------------------------------
  "instance.this":                 { path: "Instance.This", kind: "offset" },
  "instance.parent":               { path: "Instance.Parent", kind: "offset" },
  "instance.class_descriptor":     { path: "Instance.ClassDescriptor", kind: "offset" },
  "instance.class_name":           { path: "Instance.ClassName", kind: "offset" },
  "instance.class_base":           { path: "Instance.ClassBase", kind: "offset" },
  "instance.name_container":       { path: "Instance.NameContainer", kind: "offset" },
  "instance.name_in_container":    { path: "Instance.Name", kind: "offset" },
  "instance.children_start":       { path: "Instance.ChildrenStart", kind: "offset" },
  "instance.children_end":         { path: "Instance.ChildrenEnd", kind: "offset" },
  "misc.string_length":            { path: "Misc.StringLength", kind: "offset" },
  // ---- player ------------------------------------------------------------
  "player.local_player":           { path: "Player.LocalPlayer", kind: "offset" },
  "player.user_id":                { path: "Player.UserId", kind: "offset" },
  "player.display_name":           { path: "Player.DisplayName", kind: "offset" },
  "player.team":                   { path: "Player.Team", kind: "offset" },
  "player.mouse":                  { path: "Player.Mouse", kind: "offset" },
  // ---- scripts / bytecode ------------------------------------------------
  "script.guid":                   { path: "Script.GUID", kind: "offset" },
  "script.hash":                   { path: "Script.Hash", kind: "offset" },
  "local_script.guid":             { path: "LocalScript.GUID", kind: "offset" },
  "local_script.hash":             { path: "LocalScript.Hash", kind: "offset" },
  "module_script.guid":            { path: "ModuleScript.GUID", kind: "offset" },
  "module_script.hash":            { path: "ModuleScript.Hash", kind: "offset" },
  "bytecode.pointer":              { path: "ByteCode.Pointer", kind: "offset" },
  "bytecode.size":                 { path: "ByteCode.Size", kind: "offset" },
  // ---- interaction targets ----------------------------------------------
  "click_detector.max_activation_distance":   { path: "ClickDetector.MaxActivationDistance", kind: "offset" },
  "click_detector.mouse_icon":                { path: "ClickDetector.MouseIcon", kind: "offset" },
  "proximity_prompt.max_activation_distance": { path: "ProximityPrompt.MaxActivationDistance", kind: "offset" },
  "proximity_prompt.hold_duration":           { path: "ProximityPrompt.HoldDuration", kind: "offset" },
  "proximity_prompt.enabled":                 { path: "ProximityPrompt.Enabled", kind: "offset" },
  "proximity_prompt.requires_line_of_sight":  { path: "ProximityPrompt.RequiresLineOfSight", kind: "offset" },
  // ---- character / physics ----------------------------------------------
  "humanoid.health":               { path: "Humanoid.Health", kind: "offset" },
  "humanoid.max_health":           { path: "Humanoid.MaxHealth", kind: "offset" },
  "humanoid.walkspeed":            { path: "Humanoid.Walkspeed", kind: "offset" },
  "humanoid.jump_power":           { path: "Humanoid.JumpPower", kind: "offset" },
  "humanoid.hip_height":           { path: "Humanoid.HipHeight", kind: "offset" },
  "humanoid.root_part":            { path: "Humanoid.HumanoidRootPart", kind: "offset" },
  "base_part.primitive":           { path: "BasePart.Primitive", kind: "offset" },
  "primitive.position":            { path: "Primitive.Position", kind: "offset" },
  "primitive.size":                { path: "Primitive.Size", kind: "offset" },
});

/** keys the VM layer must have that the public dump does not publish */
export const UNPUBLISHED = Object.freeze([
  "script_context.global_state",   // lua_State* — resolved by probe, never by dump
  "luau.compile",
  "luau.load",
  "lua.resume",
  "lua.pcall",
  "lua.newthread",
  "lua.pushcclosurek",
  "lua.ref",
  "lua.unref",
  "lua.setfield",
  "lua.setreadonly",
  "lua.setsafeenv",
  "lua.resetthread",
  "crt.free",
  "identity.extra_space",
  "namecall.tstring",
  "gc.list_head",
]);

function pick(obj, path) {
  return path.split(".").reduce((acc, k) => (acc && typeof acc === "object" ? acc[k] : undefined), obj);
}

const hex = (n) => "0x" + n.toString(16).toUpperCase();

/**
 * @param {any} dump       parsed theo's-offsets JSON
 * @param {{strict?: boolean}} [opts]
 */
export function normalise(dump, opts = {}) {
  const strict = opts.strict !== false;
  const src = dump?.Offsets;
  if (!src || typeof src !== "object") {
    throw new Error("dump has no `Offsets` object — not a theo's-offsets file?");
  }
  const out = {};
  const missing = [];
  const zeroed = [];
  for (const [key, spec] of Object.entries(KEYMAP)) {
    const raw = pick(src, spec.path);
    if (raw === undefined || raw === null) { missing.push(`${key} <- ${spec.path}`); continue; }
    const n = Number(raw);
    if (!Number.isFinite(n)) { missing.push(`${key} <- ${spec.path} (not a number)`); continue; }
    if (n === 0) { zeroed.push(`${key} <- ${spec.path} (dump says 0 = not available)`); continue; }
    out[key] = {
      value: hex(n),
      kind: spec.kind,
      from: spec.path,
    };
  }
  if (strict && missing.length) {
    throw new Error(
      `mapping table does not match the dump (${missing.length} key(s)); a renamed or removed ` +
      `namespace must be fixed in KEYMAP, never silently dropped:\n  ` + missing.join("\n  "));
  }
  if (strict && zeroed.length) {
    // `0` is legal in the source (dumper had no value) and is simply absent here.
    // Report it, because a key that *should* have a value going absent is a signal.
    console.error(`note: ${zeroed.length} key(s) omitted because the dump reports 0:\n  ` + zeroed.join("\n  "));
  }
  return {
    schema: SCHEMA,
    source: dump.Source ?? "https://offsets.imtheo.lol",
    roblox_version: dump["Roblox Version"] ?? "unknown",
    dumper_version: dump["Dumper Version"] ?? "unknown",
    dumped_at: dump["Dumped At"] ?? "unknown",
    total_in_dump: dump["Total Offsets"] ?? Object.keys(src).length,
    note: "Generated by tools/offsets/map_offsets.mjs — edit KEYMAP / the dump, not this file.",
    offsets: Object.fromEntries(Object.entries(out).sort(([a], [b]) => a.localeCompare(b))),
  };
}

// ---------------------------------------------------------------- CLI
function main(argv) {
  const arg = (flag, dflt) => {
    const i = argv.indexOf(flag);
    return i === -1 ? dflt : argv[i + 1];
  };
  const inPath = arg("--in");
  const outPath = arg("--out");
  const toStdout = argv.includes("--stdout");
  const strict = !argv.includes("--no-strict");
  if (!inPath) {
    console.error("usage: map_offsets.mjs --in <dump.json> [--out <embedded.json> | --stdout] [--no-strict]");
    return 2;
  }
  const dump = JSON.parse(readFileSync(inPath, "utf8"));
  const mapped = normalise(dump, { strict });
  const text = JSON.stringify(mapped, null, 2) + "\n";
  if (toStdout || !outPath) process.stdout.write(text);
  else { writeFileSync(outPath, text); console.error(`wrote ${outPath} (${Object.keys(mapped.offsets).length} keys)`); }
  return 0;
}

if (import.meta.url === `file://${process.argv[1]}`) {
  process.exitCode = main(process.argv.slice(2));
}
