# Rig migration utility

Recovered from commit `09d20195`; converts strict legacy `1.0` rigs to the
canonical `2.0` schema, and validates/normalizes existing `2.0` rigs.

Close Jamma and make a verified independent backup of the complete profiles and
all referenced rigs/assets before running. Always supply absolute paths to
distinct, non-overlapping source and destination directories. Convert backups to
a separate staging directory first:

```powershell
& .\tools\rig-migrator\migrate-rig.ps1 -SourceDirectory 'C:\checkpoint\Jamma' -DestinationDirectory 'C:\checkpoint\stage' -DryRun
& .\tools\rig-migrator\migrate-rig.ps1 -SourceDirectory 'C:\checkpoint\Jamma' -DestinationDirectory 'C:\checkpoint\stage'
```

Use `-SourceFile 'C:\checkpoint\external.rig'` instead of `-SourceDirectory` for
a single rig. The historical no-argument defaults point from `Jamma` to
`Jamma-rig-cleanup`; do not use those defaults for canonical publication.

Review every warning. Legacy MIDI hardware names become generated registry IDs.
Empty station targets become null. Timing numbers are preserved: the old
`debounceSamps` field was consumed as milliseconds despite its name. Legacy `any`
capture remains disabled. Later overlapping controls are removed according to
textual group and binding order; duplicate capture selections are removed.
Files inside backup directories or with backup suffixes are explicitly skipped.
For an archival rig selected for active use, make a separate source copy outside
the backup directory and convert that copy. Unsupported rigs block the batch.

The default mode changes only rigs. `run-all.cmd` enables `-CopyProfile`, which
supports only compatible empty-take JAMs and defaults; recorded sessions and
their media require separate preservation and reconciliation.

Validate staged rigs with the real merged application parser before publishing.
For same-name divergent rigs preserve both under deliberately chosen unique
names before assembling the final canonical source. Existing destination files
are replaced transactionally with unique `.migration-backup-*` originals.
Rerunning an equivalent source preserves destination bytes. Source hashes are
checked before and after publication.

An interrupted transaction leaves `.rig-migration-transaction.json`. A dry-run
refuses it. A real run first rolls it back, then prepares the requested migration
again. Preserve the journal, stages and backups when recovery reports failure.
Recovery refuses to overwrite independently changed files or discard a journal
whose original backup is missing. Successful transactions retain destination
backups; independent profile checkpoints remain the primary rollback source.
Restore only affected files with Jamma stopped, preserving new recordings.
Legacy executables require the corresponding legacy profile checkpoint.

Run isolated regression checks (fixtures and logs are retained):

```powershell
& .\tools\rig-migrator\test-migration.ps1 -TestDirectory 'C:\workspace\migration-fixtures'
```
