cask "silkie" do
  version "0.49.2-silkie20261120.1"
  sha256 "4d1cdda99a4e29457d02c880f91cb5576f782a145b7bfab27a56b960607cb2d0"
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
