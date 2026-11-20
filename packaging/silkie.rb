cask "silkie" do
  version "0.49.2-silkie20261119.1"
  sha256 "b0532cd0b59da2f42c2283cc5e4d81f69fd746418f7b702526d2c7c7c18bf6e8"
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
  if respond_to?(:postflight_steps)
    postflight_steps do
      run "/usr/bin/xattr", args: ["-dr", "com.apple.quarantine", "{{appdir}}/silkie.app"],
                            writable_paths: ["silkie.app"], writable_base: :appdir
    end
  else
    postflight do
      system_command "/usr/bin/xattr", args: ["-dr", "com.apple.quarantine", "#{appdir}/silkie.app"]
    end
  end
  caveats <<~EOS
    Launch with silkie; companion tools use silkie-kitten.
    Configuration: ~/.config/silkie/silkie.conf.
    This build is ad-hoc signed and is not Apple-notarized, so the cask
    clears its quarantine flag after installing it.
  EOS
end
