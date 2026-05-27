.PHONY: lint lint-python lint-firmware test test-python test-mcp mcp-test build-firmware

lint: lint-python lint-firmware

lint-python:
	uv run ruff check .

lint-firmware:
	cd firmware && pio check --severity=high --fail-on-defect=high

test: test-python build-firmware

test-python:
	uv run pytest

test-mcp:
	uv run pytest tests/test_mcp_server.py

mcp-test: test-mcp

build-firmware:
	cd firmware && pio run
