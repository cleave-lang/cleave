//! EVM execution module backed by REVM.
//!
//! This is the second runtime engine that ships with Cleave. The first
//! (in `lib.rs`) is Wasmtime for Cleave-compiled WASM modules; this
//! one runs Solidity-compiled (or hand-crafted) EVM bytecode. Both
//! engines live in the same crate so a chain manifest can declare
//! `exec: EVM<runtime=REVM>` and pick this engine without an extra
//! dependency.
//!
//! # v0 simplifications
//!
//! The integration is deliberately minimal:
//!
//! * State lives in REVM's `CacheDB` over `EmptyDB`. Each `Evm`
//!   instance owns its own database; cross-engine state sharing with
//!   the Wasmtime runtime is its own follow-up issue.
//! * Contract installation goes through `install`: the bytecode is
//!   written directly into a chosen address with no constructor run.
//!   `deploy` (CREATE) is also exposed for the standard path.
//! * No JSON-RPC layer, no signature recovery, no fee market. Calls
//!   are direct: `caller`, `to`, `data`. The runtime simulates a
//!   single-threaded mempool of one transaction at a time.
//!
//! Each of those simplifications has its own future issue; this module
//! is the smallest piece that makes "Solidity bytecode runs on a chain
//! built with Cleave" demonstrably true.

use anyhow::{anyhow, Context as _, Result};
use revm::context::result::{ExecutionResult, Output};
use revm::context::{ContextTr, TxEnv};
use revm::database::CacheDB;
use revm::database_interface::EmptyDB;
use revm::primitives::hardfork::SpecId;
use revm::primitives::TxKind;
use revm::state::{AccountInfo, Bytecode};
use revm::{Context, DatabaseRef, ExecuteCommitEvm, MainBuilder, MainContext};

// Re-export the REVM primitive types our public API surface uses, so
// downstream callers (cleave-run CLI, future chain integration) do
// not need a direct REVM dependency for basic interop.
pub use revm::primitives::{Address, Bytes, StorageKey, StorageValue, U256};

/// Concrete EVM type built from REVM's mainnet handler over an
/// in-memory cached database. Held as a generic-erased field so
/// callers don't pay attention to REVM's deep generics.
type RuntimeEvm = revm::handler::MainnetEvm<
    revm::handler::MainnetContext<CacheDB<EmptyDB>>,
>;

/// EVM execution engine instance. One per logical chain in v0; the
/// future cross-VM story will likely share state across multiple
/// engine instances.
pub struct Evm {
    evm: RuntimeEvm,
}

impl Default for Evm {
    fn default() -> Self {
        Self::new()
    }
}

impl Evm {
    /// Construct a fresh EVM with an empty in-memory state.
    ///
    /// Pinned to the Cancun hardfork. We avoid the engine-default
    /// (Osaka) so that brand-new EIPs do not silently change
    /// execution semantics between REVM releases; Cancun is the most
    /// recent fork that has been live on mainnet for long enough to
    /// be load-bearing across the EVM ecosystem.
    pub fn new() -> Self {
        let ctx = Context::mainnet()
            .with_db(CacheDB::new(EmptyDB::default()))
            .modify_cfg_chained(|cfg| {
                cfg.set_spec_and_mainnet_gas_params(SpecId::CANCUN);
            });
        let evm = ctx.build_mainnet();
        Self { evm }
    }

    /// Pre-fund an account so it can pay gas. Tests and callers
    /// should fund any address they intend to use as a transaction
    /// caller; addresses with zero balance cannot pay even the
    /// minimum intrinsic gas and will see their transactions
    /// rejected during validation.
    pub fn fund(&mut self, address: Address, balance: U256) {
        let info = AccountInfo {
            balance,
            nonce: 0,
            ..AccountInfo::default()
        };
        self.evm.ctx.db_mut().insert_account_info(address, info);
    }

    /// Directly install contract bytecode at a chosen address without
    /// running a constructor. Useful for tests and for loading
    /// pre-deployed runtime bytecode.
    ///
    /// The account's nonce is set to 1 (standard for deployed
    /// contracts) and its balance to zero.
    pub fn install(&mut self, address: Address, code: impl Into<Bytes>) {
        let bytecode = Bytecode::new_raw(code.into());
        let info = AccountInfo {
            balance: U256::ZERO,
            nonce: 1,
            code_hash: bytecode.hash_slow(),
            code: Some(bytecode),
            // account_id is an internal REVM hint; None lets the
            // database assign one on first access.
            ..AccountInfo::default()
        };
        self.evm.ctx.db_mut().insert_account_info(address, info);
    }

    /// Deploy a contract via CREATE, running its deployment code. The
    /// deployment code is expected to RETURN the runtime bytecode in
    /// the standard EVM pattern.
    ///
    /// Returns the deployed contract address.
    pub fn deploy(&mut self, deployer: Address, code: impl Into<Bytes>) -> Result<Address> {
        let tx = TxEnv::builder()
            .caller(deployer)
            .kind(TxKind::Create)
            .data(code.into())
            .gas_limit(10_000_000)
            .nonce(self.account_nonce(deployer))
            .build()
            .map_err(|e| anyhow!("building deploy tx: {e:?}"))?;

        let result = self
            .evm
            .transact_commit(tx)
            .context("revm transact_commit for deploy")?;

        match result {
            ExecutionResult::Success {
                output: Output::Create(_, Some(addr)),
                ..
            } => Ok(addr),
            ExecutionResult::Success { output, .. } => {
                Err(anyhow!("deploy succeeded but no address returned: {output:?}"))
            }
            ExecutionResult::Revert { output, .. } => Err(anyhow!(
                "deploy reverted: {} bytes returned",
                output.len()
            )),
            ExecutionResult::Halt { reason, .. } => {
                Err(anyhow!("deploy halted: {reason:?}"))
            }
        }
    }

    /// Call an installed contract. Returns the output bytes the
    /// contract RETURNed (empty if the contract uses STOP).
    pub fn call(
        &mut self,
        caller: Address,
        to: Address,
        data: impl Into<Bytes>,
    ) -> Result<Bytes> {
        // Explicit Legacy tx type (0) keeps validation simple: no
        // priority-fee / blob-fee / authorization-list requirements.
        // The default tx-type derivation in REVM 40 can pick a more
        // complex type when fields are unset, which then fails build.
        let tx = TxEnv::builder()
            .tx_type(Some(0))
            .caller(caller)
            .kind(TxKind::Call(to))
            .data(data.into())
            .gas_limit(10_000_000)
            .gas_price(0)
            .nonce(self.account_nonce(caller))
            .chain_id(Some(1))
            .build()
            .map_err(|e| anyhow!("building call tx: {e:?}"))?;

        let result = self
            .evm
            .transact_commit(tx)
            .context("revm transact_commit for call")?;

        match result {
            ExecutionResult::Success { output, .. } => Ok(match output {
                Output::Call(bytes) => bytes,
                Output::Create(bytes, _) => bytes,
            }),
            ExecutionResult::Revert { output, .. } => {
                Err(anyhow!("call reverted: {} bytes returned", output.len()))
            }
            ExecutionResult::Halt { reason, .. } => {
                Err(anyhow!("call halted: {reason:?}"))
            }
        }
    }

    /// Read a storage slot from an installed contract. Returns zero
    /// for unset slots (standard EVM semantics).
    pub fn storage(&self, address: Address, slot: StorageKey) -> StorageValue {
        self.evm
            .ctx
            .db_ref()
            .storage_ref(address, slot)
            .unwrap_or(StorageValue::ZERO)
    }

    /// Current nonce of an account. Reads through the cache; returns 0
    /// for accounts that don't yet exist.
    fn account_nonce(&self, address: Address) -> u64 {
        self.evm
            .ctx
            .db_ref()
            .basic_ref(address)
            .ok()
            .flatten()
            .map(|a| a.nonce)
            .unwrap_or(0)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use revm::primitives::address;

    /// Hand-crafted EVM runtime bytecode equivalent to the Cleave
    /// counter MVP: every call increments storage slot 0 by 1 and
    /// returns the new value as a 32-byte big-endian word.
    ///
    /// Pseudo-disassembly:
    ///   PUSH1 0  SLOAD          ; stack: [count]
    ///   PUSH1 1  ADD             ; stack: [count + 1]
    ///   DUP1                     ; stack: [count+1, count+1]
    ///   PUSH1 0  SSTORE          ; stack: [count+1]; store at slot 0
    ///   PUSH1 0  MSTORE          ; stack: [];      write to memory
    ///   PUSH1 32 PUSH1 0 RETURN  ; return memory[0..32]
    const COUNTER_RUNTIME_CODE: &[u8] = &[
        0x60, 0x00, // PUSH1 0
        0x54,       // SLOAD
        0x60, 0x01, // PUSH1 1
        0x01,       // ADD
        0x80,       // DUP1
        0x60, 0x00, // PUSH1 0
        0x55,       // SSTORE
        0x60, 0x00, // PUSH1 0
        0x52,       // MSTORE
        0x60, 0x20, // PUSH1 32
        0x60, 0x00, // PUSH1 0
        0xF3,       // RETURN
    ];

    // IMPORTANT: addresses below 0x0a are reserved for EVM precompiles
    // (ECRecover, SHA256, etc.). REVM intercepts calls to those
    // addresses before any user-installed bytecode runs. Pick a target
    // safely outside that range.
    const COUNTER_ADDR: Address = address!("c0ffee0000000000000000000000000000000001");
    const CALLER_ADDR: Address = address!("000000000000000000000000000000000000fa11");

    fn read_u256_be(bytes: &Bytes) -> U256 {
        let mut padded = [0u8; 32];
        let copy_len = bytes.len().min(32);
        padded[32 - copy_len..].copy_from_slice(&bytes[..copy_len]);
        U256::from_be_bytes(padded)
    }

    fn fund_for_test(evm: &mut Evm) {
        // Give the caller enough balance to pay tx gas even when the
        // engine config enforces balance checks.
        evm.fund(CALLER_ADDR, U256::from(10_000_000_000_000_000_000u128));
    }

    #[test]
    fn install_then_call_returns_one() {
        let mut evm = Evm::new();
        fund_for_test(&mut evm);
        evm.install(COUNTER_ADDR, COUNTER_RUNTIME_CODE.to_vec());
        let out = evm.call(CALLER_ADDR, COUNTER_ADDR, Bytes::new()).unwrap();
        assert_eq!(read_u256_be(&out), U256::from(1u64));
    }

    #[test]
    fn state_persists_across_calls() {
        let mut evm = Evm::new();
        fund_for_test(&mut evm);
        evm.install(COUNTER_ADDR, COUNTER_RUNTIME_CODE.to_vec());
        for expected in 1u64..=5 {
            let out = evm.call(CALLER_ADDR, COUNTER_ADDR, Bytes::new()).unwrap();
            assert_eq!(read_u256_be(&out), U256::from(expected));
        }
        let slot0 = evm.storage(COUNTER_ADDR, StorageKey::ZERO);
        assert_eq!(slot0, StorageValue::from(5u64));
    }

    #[test]
    fn deterministic_across_two_instances() {
        let run_once = || -> Vec<U256> {
            let mut evm = Evm::new();
            fund_for_test(&mut evm);
            evm.install(COUNTER_ADDR, COUNTER_RUNTIME_CODE.to_vec());
            (0..10)
                .map(|_| {
                    let out = evm.call(CALLER_ADDR, COUNTER_ADDR, Bytes::new()).unwrap();
                    read_u256_be(&out)
                })
                .collect()
        };
        assert_eq!(run_once(), run_once());
    }

    #[test]
    fn deploy_via_create_constructor_returns_runtime_bytecode() {
        // Deployment code: copy RUNTIME bytes into memory and RETURN
        // them. Standard "no constructor" deploy pattern, hand-crafted.
        //
        //   PUSH1 17    ; runtime size
        //   PUSH1 12    ; runtime offset within this bytecode
        //   PUSH1 0     ; dest offset in memory
        //   CODECOPY    ; copy code -> memory
        //   PUSH1 17    ; runtime size
        //   PUSH1 0     ; mem offset
        //   RETURN
        //   <runtime bytes inline starting at offset 12>
        let runtime_len = COUNTER_RUNTIME_CODE.len() as u8;
        let mut deploy_code = vec![
            0x60, runtime_len, // PUSH1 <runtime length>
            0x60, 0x0C,        // PUSH1 12 (runtime offset within deploy code)
            0x60, 0x00,        // PUSH1 0  (dest in memory)
            0x39,              // CODECOPY
            0x60, runtime_len, // PUSH1 <runtime length>
            0x60, 0x00,        // PUSH1 0
            0xF3,              // RETURN
        ];
        assert_eq!(deploy_code.len(), 12);
        deploy_code.extend_from_slice(COUNTER_RUNTIME_CODE);
        assert_eq!(deploy_code.len(), 12 + COUNTER_RUNTIME_CODE.len());

        let mut evm = Evm::new();
        fund_for_test(&mut evm);
        let addr = evm.deploy(CALLER_ADDR, deploy_code).expect("deploy");

        // Now call the deployed contract; should behave like the
        // hand-installed one above.
        let out = evm.call(CALLER_ADDR, addr, Bytes::new()).unwrap();
        assert_eq!(read_u256_be(&out), U256::from(1u64));
    }

    #[test]
    fn missing_contract_call_reverts() {
        let mut evm = Evm::new();
        fund_for_test(&mut evm);
        // Don't install anything; call a random address with empty data.
        // EVM semantics: a call to an EOA / nonexistent contract is a
        // successful call with empty return data. We assert the call
        // does not error and returns nothing.
        let out = evm
            .call(CALLER_ADDR, COUNTER_ADDR, Bytes::new())
            .expect("call to empty contract is a successful no-op");
        assert_eq!(out.len(), 0);
    }
}
