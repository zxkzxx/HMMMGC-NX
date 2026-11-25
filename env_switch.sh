if [ -z "${DEVKITPRO:-}" ] || [ ! -d "$DEVKITPRO/cmake" ]; then
  if [ -d "/b/devkitPro/cmake" ]; then
    export DEVKITPRO="/b/devkitPro"
  else
    export DEVKITPRO="/opt/devkitpro"
  fi
fi
export DEVKITA64="$DEVKITPRO/devkitA64"
export PATH="$DEVKITA64/bin:$DEVKITPRO/tools/bin:/c/Program Files/Git/cmd:/c/Program Files/dotnet:$PATH"
export DOTNET_ROLL_FORWARD=Major
