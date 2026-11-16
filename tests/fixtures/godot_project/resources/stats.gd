class_name Stats
extends Resource

@export var max_health: int = 100
@export var speed: float = 200.0


func scaled_speed(factor: float) -> float:
	return speed * factor
