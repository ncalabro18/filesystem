

.PHONY

.DEFAULT

image:
	docker build -t sfs-riscv-test .

run:
	$(MAKE) image
	docker run --rm sfs-riscv-test