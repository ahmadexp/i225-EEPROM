## Summary

Describe the change and why it is needed.

## Validation

- [ ] `make clean`
- [ ] `make`
- [ ] `./i225nvm --help`
- [ ] Hardware-tested, if this changes device behavior
- [ ] Documentation or wiki updated, if user-facing behavior changed

## Safety Checklist

- [ ] No firmware binaries, private dumps, or proprietary updater files added
- [ ] Destructive paths remain gated by explicit user intent
- [ ] Tested behavior is clearly separated from inferred behavior
- [ ] Backup/restore guidance remains intact for flashing changes

