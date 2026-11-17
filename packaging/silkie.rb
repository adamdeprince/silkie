cask "silkie" do
  version "0.49.2-silkie20261117.1"
  sha256 "b96eba0004df18bb974400fc8e9d66800326a4ca756abc1d9c9d86960e3b36e2"
  url "https://brew.goblinreactor.com/casks/silkie-#{version}.zip"
  name "Silkie"
  desc "Silkie terminal emulator"
  homepage "https://silkie.goblinreactor.com/"
  depends_on arch: :arm64
  depends_on macos: :tahoe
  depends_on formula: "harfbuzz"
  depends_on formula: "libpng"
  depends_on formula: "little-cms2"
  depends_on formula: "openssl@4"
  depends_on formula: "python@3.14"
  depends_on formula: "xxhash"
  depends_on formula: "zstd"
  app "silkie.app"
  binary "#{appdir}/silkie.app/Contents/MacOS/silkie", target: "silkie"
  binary "#{appdir}/silkie.app/Contents/MacOS/kitten", target: "silkie-kitten"
  caveats <<~EOS
    Launch with silkie; companion tools use silkie-kitten.
    Configuration: ~/.config/kitty/kitty.conf.
    This build is ad-hoc signed and is not Apple-notarized.
    Approve first launch in System Settings > Privacy & Security if prompted.
  EOS
end
