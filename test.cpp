#include<iostream>
using namespace std;
// 基类
class B {
public:
  B() { cout << "B0::B()" << endl; }
  B(int a) { cout << "B1::B()" <<" "<< "a=" << a << endl; }
  B(const B& b) {cout << "B2::B()" << endl;} // 拷贝构造
  ~B() { cout << "~B()" << endl; }


};

B fun1(){
    B b(1);
    // std::cout << "b.a = " << std::endl;
    // return b;
}
int main() {
    B c(3);
    fun1();
    B b(2);
    return 0;
}