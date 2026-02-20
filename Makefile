PYTHON = python3

.PHONY: install run

install:
	$(PYTHON) -m pip install pyserial pyinputplus

run:
	$(PYTHON) SimpleSerialMaster.py
