// SPDX-License-Identifier: Apache-2.0
pragma solidity ^0.8.20;

// Minimal ERC-20 contract used as the smoke-test for the Solidity
// toolchain integration (issue #52).  Only the surface the integration
// test exercises: balanceOf, transfer, totalSupply, and a constructor
// that mints the initial supply to the deployer.
//
// This is deliberately not full ERC-20 (no approve / transferFrom / allowance
// / events) so the bytecode stays small and the test stays focused on
// proving the Solidity-on-Cleave path works.  Production ERC-20 deployments
// belong in user code, not this fixture.

contract MiniERC20 {
    mapping(address => uint256) public balances;
    uint256 public totalSupply;

    constructor(uint256 initial) {
        balances[msg.sender] = initial;
        totalSupply = initial;
    }

    function balanceOf(address who) external view returns (uint256) {
        return balances[who];
    }

    function transfer(address to, uint256 amount) external returns (bool) {
        require(balances[msg.sender] >= amount, "insufficient balance");
        balances[msg.sender] -= amount;
        balances[to] += amount;
        return true;
    }
}
