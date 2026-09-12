/**
 * @file golden.hpp
 * @brief Where the mechanics kernels' bit-for-bit cases are declared. Charter C7.
 *
 * This header holds no code. It exists because C7 -- "every numerical kernel must have a
 * fixed-input, bit-for-bit comparison case" -- is a property of the kernel **set**, not of any one
 * operator, and a claim needs somewhere to live that a gate can read.
 *
 * ## Why the claim needs a contract rather than a comment in a test
 *
 * The contract gate attributes test cases globally: every `TEST_CASE` id must be claimed by some
 * header's `@tests`, or it is reported as an orphan. That rule is what makes a deleted or renamed
 * case visible instead of silent, and it means a case with no owner is a case nothing protects.
 *
 * ## What the three cases pin
 *
 * `tests/golden/plugins/test_plugin_golden.cpp` runs each operator from fixed inputs for a fixed
 * number of steps and compares every state component **exactly** -- `==` on doubles, not `Approx`
 * -- plus one order-sensitive checksum over the whole state.
 *
 * That is a different question from the one the operators' own tests answer. Those assert against
 * closed forms (`x(t) = x0 cos(wt) + (v0/w) sin(wt)`, `s = g sin(theta) t^2 / 2`), which says the
 * **physics** is right. It does not say the arithmetic is unchanged, and for an integrator those
 * are separate: a reordered stage, a weight changed from `2.0` to `2.1`, a sign dropped from a
 * derivative, a `6.0` typed as `6.000001` -- each leaves the closed-form assertions passing inside
 * their tolerances while producing different bits. The bits are what a saved run contains, which is
 * the whole content of charter R2 and C1.
 *
 * ## What was measured rather than assumed
 *
 * The cases' sensitivity was tested by mutation, because a golden case that catches nothing is
 * decoration. Five semantic mutations -- a mis-weighted stage, a stage reading the wrong previous
 * stage, a changed combination weight, a dropped sign, a perturbed divisor -- each move the frozen
 * bits, so the cases catch them. Two mathematically equivalent rewrites (`0.5 * dt * k` written as
 * `dt * k / 2`, and the four stage derivatives summed in reverse order) produce **identical** bits
 * at these inputs, so the cases do not object to them. Both properties matter: the first is the
 * guarantee, and the second is why the expectations do not need re-recording after a harmless
 * rewrite.
 *
 * The measurement also corrected an assumption. Reversing the stage sum was expected to change the
 * bits -- floating-point addition is not associative -- and it does not, because the four stage
 * derivatives are within about `1e-3` of one another and round to the same sum either way. The
 * claim in this comment is what was measured; the earlier version of it was not.
 *
 * @ownership   pure (declares a claim about other files; owns nothing)
 * @thread      any
 * @pre         none
 * @post        none
 * @invariant   Every numerical kernel in this module appears in at least one golden case
 * @errors      noexcept
 * @complexity  --
 * @nondet      none
 * @frozen      no
 * @tests       plugin.golden.oscillator_bits, plugin.golden.incline_bits,
 *              plugin.golden.merge_bits
 */
#pragma once
