//! `cleave-run` is a small CLI front-end for `cleave-runtime`. It can
//! load a `.wasm` module produced by `cleavec --emit-wasm` (the WASM
//! path) or an EVM bytecode file (the EVM path) and call exported
//! functions / call into the contract.
//!
//! Usage:
//!
//!     cleave-run <module.wasm> <fn> [args...]
//!     cleave-run <module.wasm> --calls fn1 fn2 fn3
//!     cleave-run --evm <bytecode.hex> [--calls N]
//!
//! WASM mode: function args are parsed as decimal i64 and return values
//! print as decimal i64. State persists across calls within a single
//! invocation; nothing persists across invocations.
//!
//! EVM mode: `bytecode.hex` is either a file containing hex-encoded
//! runtime bytecode (with or without a `0x` prefix, whitespace ignored)
//! or a literal hex string starting with `0x`. The bytecode is
//! installed at a fixed test address; calls are made with empty input
//! data. With `--calls N`, the contract is called N times and each
//! return value prints as a hex line.

use std::env;
use std::fs;
use std::process::ExitCode;

use anyhow::{anyhow, Context, Result};
use cleave_runtime::evm::{Address, Bytes, StorageKey, U256};
use cleave_runtime::{Evm, Runtime};

fn print_usage(prog: &str) {
    eprintln!(
        "usage:\n  \
         {prog} <module.wasm> <fn> [arg ...]\n  \
         {prog} <module.wasm> --calls <fn1> [fn2 ...]\n  \
         {prog} --evm <bytecode.hex> [--calls N]"
    );
}

fn run(args: Vec<String>) -> Result<()> {
    let prog = args.first().cloned().unwrap_or_else(|| "cleave-run".into());

    if args.len() < 3 {
        print_usage(&prog);
        return Err(anyhow!("not enough arguments"));
    }

    if args[1] == "--evm" {
        return run_evm(&prog, &args[2..]);
    }

    let module_path = &args[1];
    let wasm = fs::read(module_path)
        .with_context(|| format!("reading wasm module '{module_path}'"))?;

    let rt = Runtime::new();
    let mut instance = rt.load(&wasm).context("instantiating module")?;

    if args[2] == "--calls" {
        let names = &args[3..];
        if names.is_empty() {
            print_usage(&prog);
            return Err(anyhow!("--calls requires at least one function name"));
        }
        for name in names {
            let value = instance.call(name, &[]).with_context(|| name.clone())?;
            println!("{name} = {value}");
        }
        return Ok(());
    }

    let fn_name = &args[2];
    let parsed_args: Vec<i64> = args[3..]
        .iter()
        .map(|s| {
            s.parse::<i64>()
                .with_context(|| format!("parsing arg '{s}' as i64"))
        })
        .collect::<Result<Vec<_>>>()?;
    let value = instance
        .call(fn_name, &parsed_args)
        .with_context(|| fn_name.clone())?;
    println!("{value}");
    Ok(())
}

/// EVM CLI mode: load hex bytecode, install at a known address, run
/// `--calls N` against it (default 1 call), print each return value.
fn run_evm(prog: &str, args: &[String]) -> Result<()> {
    if args.is_empty() {
        print_usage(prog);
        return Err(anyhow!("--evm requires a bytecode argument"));
    }
    let bytecode = load_bytecode(&args[0])?;

    let mut calls: usize = 1;
    let mut i = 1;
    while i < args.len() {
        match args[i].as_str() {
            "--calls" => {
                let n = args
                    .get(i + 1)
                    .ok_or_else(|| anyhow!("--calls requires a count"))?;
                calls = n.parse::<usize>().context("parsing --calls count")?;
                i += 2;
            }
            other => return Err(anyhow!("unknown evm flag: {other}")),
        }
    }

    // Test addresses well outside the precompile range (0x01..0x0a).
    let target: Address = "0xc0ffee0000000000000000000000000000000001"
        .parse()
        .expect("static address literal parses");
    let caller: Address = "0x000000000000000000000000000000000000fa11"
        .parse()
        .expect("static address literal parses");

    let mut evm = Evm::new();
    evm.fund(caller, U256::from(10_000_000_000_000_000_000u128));
    evm.install(target, bytecode);

    for n in 1..=calls {
        let out = evm
            .call(caller, target, Bytes::new())
            .with_context(|| format!("evm call {n}"))?;
        println!("call {n} = 0x{}", hex_encode(&out));
    }
    let slot0 = evm.storage(target, StorageKey::ZERO);
    println!("storage[0] = {slot0}");
    Ok(())
}

/// Parse a hex literal (`0x...`) or read a file containing hex.
/// Whitespace is ignored.
fn load_bytecode(spec: &str) -> Result<Vec<u8>> {
    let raw = if spec.starts_with("0x") || spec.starts_with("0X") {
        spec.to_string()
    } else {
        fs::read_to_string(spec).with_context(|| format!("reading bytecode '{spec}'"))?
    };
    let trimmed: String = raw
        .chars()
        .filter(|c| !c.is_whitespace())
        .collect();
    let trimmed = trimmed
        .strip_prefix("0x")
        .or_else(|| trimmed.strip_prefix("0X"))
        .unwrap_or(&trimmed);
    hex_decode(trimmed).context("decoding hex bytecode")
}

fn hex_decode(s: &str) -> Result<Vec<u8>> {
    if s.len() % 2 != 0 {
        return Err(anyhow!("hex string has odd length"));
    }
    let mut out = Vec::with_capacity(s.len() / 2);
    let bytes = s.as_bytes();
    for chunk in bytes.chunks(2) {
        let hi = hex_nibble(chunk[0])?;
        let lo = hex_nibble(chunk[1])?;
        out.push(hi << 4 | lo);
    }
    Ok(out)
}

fn hex_nibble(c: u8) -> Result<u8> {
    Ok(match c {
        b'0'..=b'9' => c - b'0',
        b'a'..=b'f' => 10 + c - b'a',
        b'A'..=b'F' => 10 + c - b'A',
        _ => return Err(anyhow!("invalid hex character: {:?}", c as char)),
    })
}

fn hex_encode(bytes: &[u8]) -> String {
    const HEX: &[u8; 16] = b"0123456789abcdef";
    let mut out = String::with_capacity(bytes.len() * 2);
    for &b in bytes {
        out.push(HEX[(b >> 4) as usize] as char);
        out.push(HEX[(b & 0xF) as usize] as char);
    }
    out
}

fn main() -> ExitCode {
    let args: Vec<String> = env::args().collect();
    match run(args) {
        Ok(()) => ExitCode::SUCCESS,
        Err(err) => {
            eprintln!("error: {err:#}");
            ExitCode::FAILURE
        }
    }
}
