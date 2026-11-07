// Links cascade_c (built by CMake with -DCASCADE_C_SHARED=ON). CASCADE_LIB_DIR names the folder
// that holds it; otherwise the system's library paths are searched.
fn main() {
    println!("cargo:rerun-if-env-changed=CASCADE_LIB_DIR");
    if let Ok(dir) = std::env::var("CASCADE_LIB_DIR") {
        println!("cargo:rustc-link-search=native={}", dir);
    }
    println!("cargo:rustc-link-lib=dylib=cascade_c");
}
