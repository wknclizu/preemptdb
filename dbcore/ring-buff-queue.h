template<class T>
class OneToOneQueue{

  T *data;
  volatile uint32_t startIdx{0};
  volatile uint32_t endIdx{0};
  uint32_t capacity;
public:
  OneToOneQueue() = default;
  OneToOneQueue(OneToOneQueue<T>&& q) = delete;

  void init(uint32_t _capacity){
    this->capacity = _capacity + 1;
    data = new T[capacity];
  }

  ~OneToOneQueue(){
    delete[] data;
  }

  bool isEmpty(){
    return startIdx == endIdx;
  }

  bool isFull(){
    return (endIdx + 1) % capacity == startIdx;
  }

  void pop(){
    startIdx = (startIdx + 1) % capacity;
  }

  T front(){
    return data[startIdx];
  }

  void push(T t){
    data[endIdx] = t;
    endIdx = (endIdx + 1) % capacity;
  }

};