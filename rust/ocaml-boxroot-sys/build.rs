fn build_boxroot(ocaml_path: &str) {
    let mut config = cc::Build::new();

    config.include(ocaml_path);
    config.include("vendor/boxroot/");
    config.file("vendor/boxroot/boxroot.c");
    config.file("vendor/boxroot/ocaml_hooks.c");

    config.compile("libocaml-boxroot.a");
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
    .trim()
    .to_owned()
    .split(' ')
    .map(|s| s.replace("-l", ""))
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
    println!("cargo:rerun-if-changed=vendor/boxroot/boxroot.c");
    println!("cargo:rerun-if-changed=vendor/boxroot/boxroot.h");
    println!("cargo:rerun-if-changed=vendor/boxroot/ocaml_hook.c");
    println!("cargo:rerun-if-changed=vendor/boxroot/ocaml_hook.h");
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
            if cfg!(feature = "without-ocamlopt") {
                // Use dummy include files in this case
                ocaml_path = "utils/without-ocamlopt".to_owned();
            } else {
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
    }

    build_boxroot(&ocaml_path);

    println!("cargo:rustc-link-search={}", out_dir.display());
    println!("cargo:rustc-link-lib=static=ocaml-boxroot");

    #[cfg(feature = "link-ocaml-runtime-and-dummy-program")]
    link_runtime(out_dir, &ocamlopt, &ocaml_path).unwrap();
}
