extends GutTest


func test_take_damage():
	var p = Player.make()
	p.take_damage(10)
	assert_eq(p.health, 90)


func test_inventory_add():
	var inv = Player.Inventory.new()
	inv.add("sword")
	assert_eq(inv.items.size(), 1)
