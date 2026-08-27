PYTHON = python3

# The Master takes its arguments on the command line, so there is nothing to
# run without them: make run ARGS="-p COM3 w 14 125 100"
ARGS ?=

.PHONY: install run

install:
	$(PYTHON) -m pip install -r requirements.txt

run:
	$(PYTHON) SimpleSerialMaster.py $(ARGS)
