#!/system/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# KernelSU MODULE_HOT_RUN_SCRIPT, run after KernelSU replaced the module directory.
exec sh "${0%/*}/hot-update.sh" activate
