@tool
class_name Player
extends CharacterBody2D
## The player character.

signal health_changed(new_value: int)

const SPEED := 300.0
@export var max_health: int = 100
@onready var sprite: Sprite2D = $Sprite2D
var health := max_health:
	set(value):
		health = clamp(value, 0, max_health)
		health_changed.emit(health)

enum State { IDLE, RUN }

# Moves the player each physics frame.
func _physics_process(delta: float) -> void:
	var dir := Input.get_axis("ui_left", "ui_right")
	velocity.x = dir * SPEED
	move_and_slide()
	sprite.flip_h = dir < 0
	super._physics_process(delta)

@rpc("any_peer")
func take_damage(amount: int) -> void:
	"""Reduce health."""
	health -= amount

static func make() -> Player:
	return Player.new()

func _init():
	pass

class Inventory extends RefCounted:
	var items := []
	func add(item) -> void:
		items.append(item)
