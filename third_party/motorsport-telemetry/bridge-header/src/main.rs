use std::env;
use std::error::Error;
use std::io;
use std::path::PathBuf;

fn generate() -> Result<PathBuf, Box<dyn Error>> {
    let mut arguments = env::args_os().skip(1);
    let output = arguments.next().map(PathBuf::from).ok_or_else(|| {
        io::Error::new(
            io::ErrorKind::InvalidInput,
            "usage: omatrack-bridge-header OUTPUT",
        )
    })?;
    if arguments.next().is_some() {
        return Err(io::Error::new(
            io::ErrorKind::InvalidInput,
            "usage: omatrack-bridge-header OUTPUT",
        )
        .into());
    }

    let bridge_directory = PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("../bridge");
    let config_path = bridge_directory.join("cbindgen.toml");
    let config = cbindgen::Config::from_file(&config_path).map_err(|error| {
        io::Error::new(
            io::ErrorKind::InvalidData,
            format!("{}: {error}", config_path.display()),
        )
    })?;
    cbindgen::generate_with_config(&bridge_directory, config)?.write_to_file(&output);
    Ok(output)
}

fn main() {
    match generate() {
        Ok(output) => println!("generated {}", output.display()),
        Err(error) => {
            eprintln!("omatrack-bridge-header: {error}");
            std::process::exit(1);
        }
    }
}
