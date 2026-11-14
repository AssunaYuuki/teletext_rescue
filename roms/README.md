# BBC Micro ROMs

The BBC Micro window of Teletext Rescue (telesoftware from Ceefax) needs the two ROMs of a Model B:

| file | ROM | size | CRC32 |
|------|-----|------|-------|
| `os12.rom` | Acorn MOS 1.20 | 16384 | `3c14fc70` |
| `basic2.rom` | BBC BASIC II | 16384 | `79434781` |

They are Acorn's ROMs as BBC Micro emulators distribute them (jsbeeb `public/roms/os.rom`, `BASIC.ROM`).
The installer, the Windows zip and the Linux packages put them in `roms` next to the program; the macOS app keeps
them in `Teletext Rescue.app/Contents/Resources/roms`.
