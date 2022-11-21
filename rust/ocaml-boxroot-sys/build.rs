/* SPDX-License-Identifier: MIT */

fn exec_cmd(cmd: &str, args: &[&str]) -> Option<String> {
    Some(
        std::str::from_utf8(
            std::process::Command::new(cmd)
                .args(args)
                .output()
                .ok()?
                .stdout
                .as_ref(),
        )
        .ok()?
        .trim()
        .to_owned(),
    )
}

fn exec_cmd_not_empty(cmd: &str, args: &[&str]) -> Option<String> {
    exec_cmd(cmd, args).filter(|ref s| !s.is_empty())
}

#[cfg(feature = "bundle-boxroot")]
fn build_boxroot() {
    println!("cargo:rerun-if-changed=vendor/boxroot/boxroot.c");
    println!("cargo:rerun-if-changed=vendor/boxroot/boxroot.h");
    println!("cargo:rerun-if-changed=vendor/boxroot/ocaml_hook.c");
    println!("cargo:rerun-if-changed=vendor/boxroot/ocaml_hook.h");
    println!("cargo:rerun-if-env-changed=OCAMLOPT");
    println!("cargo:rerun-if-env-changed=OCAML_WHERE_PATH");

    let out_dir = std::path::PathBuf::from(std::env::var("OUT_DIR").unwrap());
    let ocaml_version = std::env::var("OCAML_VERSION");
    let ocaml_where_path = std::env::var("OCAML_WHERE_PATH");

    let version: String;
    let ocaml_path: String;
    let ocamlopt = if let Ok(ocamlopt) = std::env::var("OCAMLOPT") {
        Some(ocamlopt)
    } else {
        exec_cmd_not_empty("which", &["ocamlopt"])
    };

    let ocamlopt = if let Some(ocamlopt) = ocamlopt {
        ocamlopt
    } else {
        let esy = exec_cmd_not_empty("which", &["esy"]).expect("which esy");
        exec_cmd_not_empty(&esy, &["which", "ocamlopt"]).expect("esy which ocamlopt")
    };

    match (ocaml_version, ocaml_where_path) {
        (Ok(ver), Ok(path)) => {
            version = ver;
            ocaml_path = path;
        }
        _ => {
            version = exec_cmd_not_empty(&ocamlopt, &["-version"]).expect("ocamlopt -version");
            ocaml_path = exec_cmd_not_empty(&ocamlopt, &["-where"]).expect("ocamlopt -where");
        }
    }

    let mut config = cc::Build::new();

    config.include(&ocaml_path);
    config.include("vendor/boxroot/");
    config.file("vendor/boxroot/boxroot.c");
    config.file("vendor/boxroot/ocaml_hooks.c");
    config.file("vendor/boxroot/platform.c");

    config.compile("libocaml-boxroot.a");

    println!("cargo:rustc-link-search={}", out_dir.display());
    println!("cargo:rustc-link-lib=static=ocaml-boxroot");

    #[cfg(feature = "link-ocaml-runtime-and-dummy-program")]
    link_runtime(out_dir, &ocamlopt, &ocaml_path).unwrap();
}

#[cfg(feature = "link-ocaml-runtime-and-dummy-program")]
fn link_runtime(
    out_dir: std::path::PathBuf,
    ocamlopt: &str,
    ocaml_path: &str,
) -> std::io::Result<()> {
    use std::io::Write;

    let mut f = std::fs::File::create(out_dir.join("empty.ml")).unwrap();
    write!(f, "")?;

    assert!(std::process::Command::new(&ocamlopt)
        .args(&["-output-obj", "-o"])
        .arg(out_dir.join("dummy.o"))
        .arg(out_dir.join("empty.ml"))
        .status()?
        .success());

    let ar = std::env::var("AR").unwrap_or_else(|_| "ar".to_string());
    assert!(std::process::Command::new(&ar)
        .arg("rcs")
        .arg(out_dir.join("libdummy.a"))
        .arg(out_dir.join("dummy.o"))
        .status()?
        .success());

    let cc_libs: Vec<String> = std::str::from_utf8(
        std::process::Command::new(&ocamlopt)
            .args(&["-config-var", "native_c_libraries"])
            .output()
            .unwrap()
            .stdout
            .as_ref(),
    )
    .unwrap()
    .to_owned()
    .split_whitespace()
    .map(|s| {
        assert!(&s[0..2] == "-l");
        String::from(&s[2..])
    })
    .collect();

    for lib in cc_libs {
        println!("cargo:rustc-link-lib={}", lib);
    }

    println!("cargo:rustc-link-search={}", out_dir.display());
    println!("cargo:rustc-link-lib=static=dummy");

    println!("cargo:rustc-link-search={}", ocaml_path);
    println!("cargo:rustc-link-lib=dylib=asmrun");

    Ok(())
}

fn main() {
    #[cfg(feature = "bundle-boxroot")]
    build_boxroot();
}
