use std::io::{BufRead, Write};

const CC_LIB_PREFIX: &str = "NATIVECCLIBS=";

fn cc_libs(ocaml_path: &str) -> std::io::Result<Vec<String>> {
    let path = format!("{}/Makefile.config", ocaml_path);
    let f = std::io::BufReader::new(std::fs::File::open(path)?);

    for line in f.lines() {
        if let Ok(line) = line {
            if line.starts_with(CC_LIB_PREFIX) {
                let line: Vec<_> = line.split("=").collect();
                let line = line[1].split(" ");
                return Ok(line
                    .filter_map(|x| {
                        if x == "" {
                            None
                        } else {
                            Some(x.replace("-l", ""))
                        }
                    })
                    .collect());
            }
        }
    }
    Ok(vec![])
}

fn build_boxroot(ocaml_path: &str) {
    let mut config = cc::Build::new();

    config.include(ocaml_path);
    config.include("../../boxroot/");
    config.file("../../boxroot/boxroot.c");

    config.compile("libocaml-boxroot.a");
}

fn link_runtime(
    out_dir: std::path::PathBuf,
    ocamlopt: String,
    ocaml_path: &str,
) -> std::io::Result<()> {
    let mut f = std::fs::File::create(out_dir.join("runtime.ml")).unwrap();
    write!(f, "")?;

    assert!(std::process::Command::new(&ocamlopt)
        .args(&["-output-complete-obj", "-o"])
        .arg(out_dir.join("rt.o"))
        .arg(out_dir.join("runtime.ml"))
        .status()?
        .success());

    let ar = std::env::var("AR").unwrap_or_else(|_| "ar".to_string());
    assert!(std::process::Command::new(&ar)
        .arg("rcs")
        .arg(out_dir.join("libruntime.a"))
        .arg(out_dir.join("rt.o"))
        .status()?
        .success());

    for lib in cc_libs(ocaml_path)? {
        println!("cargo:rustc-link-lib={}", lib);
    }

    println!("cargo:rustc-link-search={}", out_dir.display());
    println!("cargo:rustc-link-lib=static=runtime");

    Ok(())
}

fn main() {
    println!("cargo:rerun-if-changed=../../boxroot/");
    println!("cargo:rerun-if-env-changed=OCAMLOPT");
    println!("cargo:rerun-if-env-changed=OCAML_WHERE_PATH");

    let out_dir = std::path::PathBuf::from(std::env::var("OUT_DIR").unwrap());
    let ocaml_where_path = std::env::var("OCAML_WHERE_PATH");
    let ocamlopt = std::env::var("OCAMLOPT").unwrap_or_else(|_| "ocamlopt".to_string());

    let ocaml_path: String;

    match ocaml_where_path {
        Ok(path) => {
            ocaml_path = path;
        }
        _ => {
            ocaml_path = std::str::from_utf8(
                std::process::Command::new(&ocamlopt)
                    .arg("-where")
                    .output()
                    .unwrap()
                    .stdout
                    .as_ref(),
            )
            .unwrap()
            .trim()
            .to_owned();
        }
    }

    build_boxroot(&ocaml_path);

    println!("cargo:rustc-link-search={}", out_dir.display());
    println!("cargo:rustc-link-lib=static=ocaml-boxroot");

    if cfg!(feature = "link-ocaml-runtime") {
        let bin_path = format!("{}/../../bin/ocamlopt", ocaml_path);

        link_runtime(out_dir, bin_path, &ocaml_path).unwrap();
    }
}
