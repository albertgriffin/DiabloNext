# Render Telemetry Hellfire Demo 1

Current-build Hellfire demo recorded on 2026-05-19 for repeatable render telemetry and regression measurements.

## Files

- `demo_1.dmo`: recorded input stream.
- `single_0.hsv`: starting Hellfire save. Restore this before every replay.
- `demo_1_reference_single_0.hsv`: expected end-state save for timedemo validation.

## Validated Baseline

The fixture was validated with `--demo 1 --timedemo`; compositor off, compositor on, and tint + outline diagnostic replays all ended with:

```text
Timedemo: Same outcome as initial run. :)
```

The original matched logs live locally under `/Users/agriffin/Desktop/render-telemetry-comparison/`:

- `demo-replay-hellfire-2-compositor-off.log`
- `demo-replay-hellfire-2-compositor-on.log`
- `demo-replay-hellfire-2-diagnostic.log`

## Replay Notes

When `--data-dir` points at the MPQ folder, the Hellfire mod data is not found from the app bundle. Prepare a scratch save directory with `mods/Hellfire` copied from the repository:

```sh
fixture="$PWD/test/fixtures/timedemo/RenderTelemetryHellfireDemo1"
scratch="/tmp/diablonext-render-telemetry-demo1"
config="/tmp/diablonext-render-telemetry-demo1-config"

rm -rf "$scratch" "$config"
mkdir -p "$scratch" "$config" "$scratch/mods"
cp "$fixture/demo_1.dmo" "$scratch/"
cp "$fixture/single_0.hsv" "$scratch/"
cp "$fixture/demo_1_reference_single_0.hsv" "$scratch/"
rsync -a "$PWD/mods/Hellfire" "$scratch/mods/"
cp "$HOME/Library/Application Support/diasurgical/devilution/diablo.ini" "$config/diablo.ini"
```

Before each replay, restore the starting save:

```sh
cp "$fixture/single_0.hsv" "$scratch/single_0.hsv"
```

Run replay from the repository root, adjusting the config options for the scenario under test:

```sh
./build/devilutionx.app/Contents/MacOS/devilutionx \
  --data-dir "$HOME/Library/Application Support/diasurgical/devilution" \
  --save-dir "$scratch" \
  --config-dir "$config" \
  --hellfire --demo 1 --timedemo \
  --log-to-file "$scratch/render-telemetry.log" -n
```

Use the `Timedemo: Same outcome as initial run. :)` line as the fixture validity check before trusting performance numbers.
