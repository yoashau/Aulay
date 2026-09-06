#!/bin/sh

set -eu
cd "$(dirname "$0")/.."

FILES="Aulay.cpp SettingsUtil.hpp"

COPYRIGHT_HOLDER="Richard Yu <yurichard3839@gmail.com>"
PKG_NAME="Aulay"

xgettext -o translate/source/messages.pot --c++ --add-comments=/ --keyword=_ --keyword=C_:1c,2 --copyright-holder="$COPYRIGHT_HOLDER" --package-name="$PKG_NAME" $FILES
