//! `cleave-run` is a small CLI front-end for `cleave-runtime`. It loads
//! a `.wasm` module produced by `cleavec --emit-wasm` and calls one or
//! more exported functions with shared state.
//!
//! Usage:
//!
//!     cleave-run <module.wasm> <fn> [args...]
//!     cleave-run <module.wasm> --calls fn1 fn2 fn3
//!
//! All function args are parsed as decimal i64. Return values are
//! printed one per line. State persists across calls within a single
//! `cleave-run` invocation; nothing is persisted across invocations.

use std::env;
use std::fs;
use std::process::ExitCode;

use anyhow::{anyhow, Context, Result};
use cleave_runtime::Runtime;

fn print_usage(prog: &str) {
    eprintln!(
        "usage:\n  {prog} <module.wasm> <fn> [arg ...]\n  {prog} <module.wasm> --calls <fn1> [fn2 ...]"
    );
}

fn run(args: Vec<String>) -> Result<()> {
    let prog = args.first().cloned().unwrap_or_else(|| "cleave-run".into());

    if args.len() < 3 {
        print_usage(&prog);
        return Err(anyhow!("not enough arguments"));
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
