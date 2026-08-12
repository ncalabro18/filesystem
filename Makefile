

.DEFAULT = run_tests

# test_image creates a file but isn't in the source tree so its marked as phony
.PHONY = run_tests test_image test


test_image:
	docker build -t sfs-riscv-test .

test:
	docker run --rm sfs-riscv-test

run_tests:
	$(MAKE) test_image
	$(MAKE) test
