//! Solidity toolchain integration (issue #52).
//!
//! Compiles `tests/fixtures/MiniERC20.sol` with the system `solc`,
//! deploys via `Evm::deploy`, then exercises the standard ERC-20 surface
//! (balanceOf, transfer) using hand-rolled ABI encoding.  Skips cleanly
//! with a printed notice if `solc` is not on the PATH.
//!
//! What this test proves: a real Solidity contract, compiled by the
//! real Solidity compiler, runs end-to-end on the EVM engine in
//! `cleave-runtime`.  No precompiled bytecode constants and no
//! hand-crafted opcodes.

use std::path::PathBuf;
use std::process::Command;

use anyhow::{anyhow, Context, Result};
use cleave_runtime::evm::{Address, Bytes, U256};
use cleave_runtime::Evm;

// ============== environment + skip helpers ==============

fn workspace_root() -> PathBuf {
    PathBuf::from(env!("CARGO_MANIFEST_DIR"))
}

fn erc20_source() -> PathBuf {
    workspace_root().join("tests/fixtures/MiniERC20.sol")
}

fn ensure_solc_or_skip(test_name: &str) -> bool {
    match Command::new("solc").arg("--version").output() {
        Ok(out) if out.status.success() => true,
        _ => {
            eprintln!(
                "{test_name}: skipping; `solc` not found on PATH. \
                 Install via `brew install solidity` or `apt-get install solc`."
            );
            false
        }
    }
}

// ============== solc shell-out ==============

/// Compile a Solidity source file with `solc --bin --optimize` and
/// return the deployment bytecode for the named contract.
fn compile_contract(source: &PathBuf, contract_name: &str) -> Result<Vec<u8>> {
    let output = Command::new("solc")
        .args(["--bin", "--optimize", "--no-color"])
        .arg(source)
        .output()
        .context("running solc")?;

    if !output.status.success() {
        return Err(anyhow!(
            "solc failed:\n{}",
            String::from_utf8_lossy(&output.stderr)
        ));
    }

    let stdout = String::from_utf8(output.stdout).context("solc stdout was not utf-8")?;
    extract_binary(&stdout, contract_name)
}

/// Parse the textual output of `solc --bin`.  The format we look for:
///
///     ======= path/file.sol:ContractName =======
///     Binary:
///     608060405234801561...
fn extract_binary(stdout: &str, contract_name: &str) -> Result<Vec<u8>> {
    let header_suffix = format!(":{contract_name} =======");
    enum Phase { Header, Binary, Hex }
    let mut phase = Phase::Header;
    for raw in stdout.lines() {
        let line = raw.trim();
        match phase {
            Phase::Header => {
                if raw.starts_with("=======") && raw.ends_with(&header_suffix) {
                    phase = Phase::Binary;
                }
            }
            Phase::Binary => {
                if line.starts_with("Binary:") {
                    phase = Phase::Hex;
                }
            }
            Phase::Hex => {
                if !line.is_empty() {
                    return hex_decode(line);
                }
            }
        }
    }
    Err(anyhow!(
        "could not find binary for contract '{contract_name}' in solc output"
    ))
}

fn hex_decode(s: &str) -> Result<Vec<u8>> {
    let s = s.strip_prefix("0x").or_else(|| s.strip_prefix("0X")).unwrap_or(s);
    if !s.len().is_multiple_of(2) {
        return Err(anyhow!("hex string has odd length"));
    }
    let bytes = s.as_bytes();
    let mut out = Vec::with_capacity(s.len() / 2);
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

// ============== ABI encoding helpers ==============
//
// Hand-rolled for the three function selectors this test uses.  Each
// selector is the first 4 bytes of keccak256("name(types)"):
//
//     balanceOf(address)              -> 0x70a08231
//     transfer(address,uint256)       -> 0xa9059cbb
//     totalSupply()                   -> 0x18160ddd
//
// Pulling in alloy-sol-types would handle this generically but adds a
// non-trivial dep for ~30 lines of hand-rolling.  Worth revisiting if
// the test surface grows.

const SEL_BALANCE_OF: [u8; 4] = [0x70, 0xa0, 0x82, 0x31];
const SEL_TRANSFER: [u8; 4]   = [0xa9, 0x05, 0x9c, 0xbb];
const SEL_TOTAL_SUPPLY: [u8; 4] = [0x18, 0x16, 0x0d, 0xdd];

/// Big-endian 32-byte word, left-padded with zeros for shorter inputs.
fn encode_u256(value: U256) -> [u8; 32] {
    value.to_be_bytes()
}

/// 32-byte word: 12 bytes of zero padding + 20-byte address.
fn encode_address(addr: Address) -> [u8; 32] {
    let mut out = [0u8; 32];
    out[12..].copy_from_slice(addr.as_slice());
    out
}

/// ABI for the deployment constructor `constructor(uint256 initial)`:
/// just the 32-byte initial supply appended to the bytecode.
fn encode_deploy_call(bytecode: Vec<u8>, initial_supply: U256) -> Vec<u8> {
    let mut out = bytecode;
    out.extend_from_slice(&encode_u256(initial_supply));
    out
}

fn encode_balance_of(who: Address) -> Vec<u8> {
    let mut out = Vec::with_capacity(4 + 32);
    out.extend_from_slice(&SEL_BALANCE_OF);
    out.extend_from_slice(&encode_address(who));
    out
}

fn encode_transfer(to: Address, amount: U256) -> Vec<u8> {
    let mut out = Vec::with_capacity(4 + 32 + 32);
    out.extend_from_slice(&SEL_TRANSFER);
    out.extend_from_slice(&encode_address(to));
    out.extend_from_slice(&encode_u256(amount));
    out
}

fn encode_total_supply() -> Vec<u8> {
    SEL_TOTAL_SUPPLY.to_vec()
}

/// Decode a 32-byte big-endian uint256 return value.
fn decode_u256(bytes: &Bytes) -> U256 {
    let mut padded = [0u8; 32];
    let copy_len = bytes.len().min(32);
    padded[32 - copy_len..].copy_from_slice(&bytes[..copy_len]);
    U256::from_be_bytes(padded)
}

// ============== the tests ==============

#[test]
fn erc20_deploys_and_transfers() {
    if !ensure_solc_or_skip("erc20_deploys_and_transfers") {
        return;
    }

    // Compile the fixture.
    let bytecode = compile_contract(&erc20_source(), "MiniERC20")
        .expect("compile MiniERC20.sol");
    assert!(!bytecode.is_empty(), "compiled bytecode should be non-empty");

    // Deploy with constructor arg: initial supply = 1_000_000.
    let alice: Address = "0xaaaa000000000000000000000000000000000001"
        .parse()
        .expect("static address literal parses");
    let bob: Address = "0xbbbb000000000000000000000000000000000002"
        .parse()
        .expect("static address literal parses");

    let initial_supply = U256::from(1_000_000u64);
    let deploy_calldata = encode_deploy_call(bytecode, initial_supply);

    let mut evm = Evm::new();
    evm.fund(alice, U256::from(10_000_000_000_000_000_000u128));
    evm.fund(bob, U256::from(10_000_000_000_000_000_000u128));

    let contract_addr = evm
        .deploy(alice, deploy_calldata)
        .expect("MiniERC20 deploys");

    // totalSupply() should equal the constructor arg.
    let supply_bytes = evm
        .call(alice, contract_addr, encode_total_supply())
        .expect("totalSupply call succeeds");
    assert_eq!(decode_u256(&supply_bytes), initial_supply);

    // balanceOf(alice) should equal initial supply (alice deployed).
    let alice_balance = evm
        .call(alice, contract_addr, encode_balance_of(alice))
        .expect("balanceOf(alice) call succeeds");
    assert_eq!(decode_u256(&alice_balance), initial_supply);

    // balanceOf(bob) should be zero.
    let bob_balance = evm
        .call(alice, contract_addr, encode_balance_of(bob))
        .expect("balanceOf(bob) call succeeds");
    assert_eq!(decode_u256(&bob_balance), U256::ZERO);

    // Transfer 100 from alice to bob.
    let transfer_amount = U256::from(100u64);
    let transfer_result = evm
        .call(alice, contract_addr, encode_transfer(bob, transfer_amount))
        .expect("transfer call succeeds");
    // transfer() returns bool; 32-byte word with the LSB = 1.
    assert_eq!(decode_u256(&transfer_result), U256::from(1u64));

    // Re-check balances.
    let alice_after = evm
        .call(alice, contract_addr, encode_balance_of(alice))
        .expect("balanceOf(alice) post-transfer");
    let bob_after = evm
        .call(alice, contract_addr, encode_balance_of(bob))
        .expect("balanceOf(bob) post-transfer");
    assert_eq!(decode_u256(&alice_after), initial_supply - transfer_amount);
    assert_eq!(decode_u256(&bob_after), transfer_amount);
}

#[test]
fn erc20_transfer_reverts_when_insufficient_balance() {
    if !ensure_solc_or_skip("erc20_transfer_reverts_when_insufficient_balance") {
        return;
    }

    let bytecode = compile_contract(&erc20_source(), "MiniERC20")
        .expect("compile MiniERC20.sol");

    let alice: Address = "0xaaaa000000000000000000000000000000000001"
        .parse()
        .unwrap();
    let bob: Address = "0xbbbb000000000000000000000000000000000002"
        .parse()
        .unwrap();

    let mut evm = Evm::new();
    evm.fund(alice, U256::from(10_000_000_000_000_000_000u128));
    evm.fund(bob, U256::from(10_000_000_000_000_000_000u128));

    let deploy_calldata = encode_deploy_call(bytecode, U256::from(100u64));
    let contract_addr = evm.deploy(alice, deploy_calldata).expect("deploy");

    // Bob has zero balance; transferring 1 from bob should revert.
    let err = evm
        .call(bob, contract_addr, encode_transfer(alice, U256::from(1u64)))
        .expect_err("transfer with insufficient balance should revert");
    let msg = format!("{err:#}");
    assert!(
        msg.contains("revert"),
        "error should indicate a revert: {msg}"
    );
}
