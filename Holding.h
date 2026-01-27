#pragma once
class Holding {
public:
	double price;
	int volume;

	Holding() = default;
	Holding(double price, int volume);
};