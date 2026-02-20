"""Pluggable player strategies for Scotland Yard."""

from strategies.base import Strategy
from strategies.random_strategy import RandomStrategy
from strategies.human import HumanStrategy

__all__ = ["Strategy", "RandomStrategy", "HumanStrategy"]
