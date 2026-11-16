// Reuse Silkie's generated Unicode data instead of maintaining a second width table.
use std::{env, fs, path::PathBuf};
fn body<'a>(text: &'a str, name: &str) -> &'a str {
    text.split_once(&format!(" {name}["))
        .expect("Unicode table")
        .1
        .split_once("= {")
        .expect("Unicode table initializer")
        .1
        .split_once("};")
        .expect("Unicode table terminator")
        .0
}
fn main() {
    let path = PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("../../kitty/char-props-data.h");
    println!("cargo:rerun-if-changed={}", path.display());
    let text = fs::read_to_string(path).expect("read Silkie Unicode data");
    let mut output = String::from("// Generated from kitty/char-props-data.h, GPL-3.0.\n");
    for (source, dest) in [("CharProps_t1", "WIDTH_T1"), ("CharProps_t2", "WIDTH_T2")] {
        let values: Vec<u8> = body(&text, source)
            .split(',')
            .filter_map(|n| n.trim().parse().ok())
            .collect();
        output.push_str(&format!("const {dest}: &[u8] = &{values:?};\n"));
    }
    let widths: Vec<i8> = body(&text, "CharProps_t3")
        .split(".shifted_width=")
        .skip(1)
        .map(|part| part.split(',').next().unwrap().parse::<i8>().unwrap() - 4)
        .collect();
    output.push_str(&format!("const WIDTHS: &[i8] = &{widths:?};\n"));
    fs::write(
        PathBuf::from(env::var_os("OUT_DIR").unwrap()).join("widths.rs"),
        output,
    )
    .unwrap();
}
