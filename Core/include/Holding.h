#pragma once
class Holding {
public:
	double price;
	unsigned int volume;

	Holding() = default;
	Holding(double price, unsigned int volume);
};