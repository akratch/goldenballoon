const attested = process.env.MDKR_DEDICATED_TEST_DESKTOP === "1";

if (!attested) {
  console.error(
    "party-service-test-safety: refused to start service tests on an " +
    "interactive workstation; MDKR_DEDICATED_TEST_DESKTOP=1 must be supplied " +
    "by a human-confirmed isolated test host",
  );
  process.exit(2);
}
