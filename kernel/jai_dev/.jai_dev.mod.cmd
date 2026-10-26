savedcmd_jai_dev.mod := printf '%s\n'   jai_dev.o | awk '!x[$$0]++ { print("./"$$0) }' > jai_dev.mod
