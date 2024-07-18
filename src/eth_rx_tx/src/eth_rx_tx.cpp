#include <iostream>
#include <ros/ros.h>
#include <unistd.h>
#include <boost/asio.hpp>
#include <geometry_msgs/Twist.h>
#include <std_msgs/Bool.h>
#include <std_msgs/ByteMultiArray.h>
#include "umba_crc_table.h"

using boost::asio::ip::tcp;
using boost::asio::ip::address;

// размеры пакетов данных в нормальном режиме
#define DATA_FROM_TCP_SIZE                     101  // [6*8b VEL][6*8b ROT][1b FIRE][4b KeepAL]   // 101b
#define DATA_TO_TCP_SIZE                        24  // [20b DATA][4b KeepAL]                      // 24b
#define DATA_TO_CMD_VEL_TOPIC_SIZE              48  // [6*8b VEL]
#define DATA_TO_CAM_ROT_TOPIC_SIZE              48  // [6*8b ROT]
#define DATA_FROM_RESERVED_TOPIC_SIZE           20  // [20b DATA] // пустышка

#define TO_CMD_VEL_TOPIC_NAME                 "cmd_vel"
#define TO_CAM_ROT_TOPIC_NAME                 "cam_rot"
#define TO_CMD_FIRE_TOPIC_NAME                "fire"
#define FROM_RESERVED_TOPIC_NAME              "fromReserved"

class Session
{
public:
  Session(boost::asio::io_context& io_context)
  : socket_(io_context) {
    toCmdVelPub              = node.advertise<geometry_msgs::Twist>(TO_CMD_VEL_TOPIC_NAME, 0);
    toCamRotPub              = node.advertise<geometry_msgs::Twist>(TO_CAM_ROT_TOPIC_NAME, 0);
    toCmdFirePub             = node.advertise<std_msgs::Bool>(TO_CMD_FIRE_TOPIC_NAME, 0);
    fromReservedSub          = node.subscribe<std_msgs::ByteMultiArray>(FROM_RESERVED_TOPIC_NAME, 0, 
        &Session::from_reserved_callback, this);

    memset(dataFromTCP,      0, sizeof(dataFromTCP));
    memset(dataToTCP,        0, sizeof(dataToTCP));
    memset(dataToCmdVel,     0, sizeof(dataToCmdVel));
    memset(dataToCamRot,     0, sizeof(dataToCamRot));
    memset(dataFromReserved, 0, sizeof(dataFromReserved));
  }

  tcp::socket& socket()
  {
    return socket_;
  }

  void async_read()
  {
    socket_.async_read_some(boost::asio::buffer(dataFromTCP, DATA_FROM_TCP_SIZE),
        boost::bind(&Session::tcp_handle_receive, this,
          boost::asio::placeholders::error,
          boost::asio::placeholders::bytes_transferred));
  }

  void async_write()
  {
    boost::asio::async_write(socket_,
        boost::asio::buffer(dataToTCP, DATA_TO_TCP_SIZE),
        boost::bind(&Session::handle_write, this,
          boost::asio::placeholders::error,
          boost::asio::placeholders::bytes_transferred));
  }

private:
  ros::NodeHandle node;
  tcp::socket socket_;

  // данные при нормальном режиме работы
  uint8_t dataFromTCP[DATA_FROM_TCP_SIZE];
  uint8_t dataToTCP[DATA_TO_TCP_SIZE];

  uint8_t dataToCmdVel[DATA_TO_CMD_VEL_TOPIC_SIZE];
  uint8_t dataToCamRot[DATA_TO_CAM_ROT_TOPIC_SIZE];
  uint8_t dataFromReserved[DATA_FROM_RESERVED_TOPIC_SIZE];

  uint8_t cmd_fire = 0;

  // остальные переменные
  ros::Publisher toCmdVelPub;
  ros::Publisher toCamRotPub;
  ros::Publisher toCmdFirePub;
  ros::Subscriber fromReservedSub;

  uint32_t send_count_TCP   = 0;
  uint32_t recvd_count_TCP  = 0;
  
  struct currentState_{
    uint8_t keepalive[4]    = {0};
  };
  currentState_ currentState;

  /*
    обработчик сообщений от сети по протоколу TCP.
    полученные данные от сети по протоколу TCP "расфасовываются" в переменные 
    для дальнейшей отправки ROS-топики
  */
  void tcp_handle_receive(const boost::system::error_code& error,
      size_t bytes_transferred)
  {
    if (!error)
    {
      if (bytes_transferred != DATA_FROM_TCP_SIZE){
        memset(dataFromTCP, 0, sizeof(dataFromTCP));
        async_read();
        return;
      }
      recvd_count_TCP++;
      printGetFromTCPData(bytes_transferred);

      memcpy(dataToCmdVel, dataFromTCP, sizeof(dataToCmdVel));
      memcpy(dataToCamRot, &dataFromTCP[sizeof(dataToCmdVel)], sizeof(dataToCamRot));
      memcpy(&cmd_fire, &dataFromTCP[sizeof(dataToCmdVel) + sizeof(dataToCamRot)], sizeof(cmd_fire));
      memcpy(currentState.keepalive, &dataFromTCP[sizeof(dataToCmdVel) + sizeof(dataToCamRot) + sizeof(cmd_fire)], sizeof(currentState.keepalive));
      
      sendMsgToCmdVel();
      sendMsgToCamRot();
      sendMsgToCmdFire();
      sendMsgToTCP();

      async_read();
    }
    else
    {
      delete this;
    }
  }
  
  // обработчик сообщений от ROS-топика "fromReserved". 
  // полученные данные с ROS-топика "расфасовываются" в массив для дальнейшей отправки по TCP
  void from_reserved_callback(const std_msgs::ByteMultiArray::ConstPtr& recvdMsg){
    if (recvdMsg->data.size() == DATA_FROM_RESERVED_TOPIC_SIZE){
      for (int i = 0; i < recvdMsg->data.size(); i++){
        dataFromReserved[i] = recvdMsg->data[i];
      }
    }
  }

  /*
    Вывод в консоль полученных по TCP данных
  */
  void printGetFromTCPData(uint32_t bytes_transferred){
    std::cout << "\n\033[1;36mRECVD FROM TCP bytes_transferred = \033[0m" << bytes_transferred << std::endl;
    std::cout << "recvd_count_TCP = " << recvd_count_TCP << std::endl;
    for (int i = 0; i < bytes_transferred; i++) { 
      printf("[%u]", dataFromTCP[i]);
    }
    printf("\n");
  }

  /*
    Вывод в консоль отправляемых по TCP данных
  */
  void printSendToTCPData(uint32_t bytes_transferred){
    std::cout << "\n\033[1;36mSEND TO TCP bytes_transferred = \033[0m" << bytes_transferred << std::endl;
    std::cout << "send_count_TCP = " << send_count_TCP << std::endl;
    for (int i = 0; i < bytes_transferred; i++){
      printf("[%u]", dataToTCP[i]);
    }
    printf("\n");
  }

  double getDoubleFromBytes(uint8_t *data){
    double d = 0;
    memcpy(&d, data, 8);
    return d;
  }

  //отправка пакета в топик "cmd_vel"
  void sendMsgToCmdVel(){
    geometry_msgs::Twist msg;
    msg.linear.x  = getDoubleFromBytes(&dataToCmdVel[0]);
    msg.linear.y  = getDoubleFromBytes(&dataToCmdVel[8]);
    msg.linear.z  = getDoubleFromBytes(&dataToCmdVel[16]);
    msg.angular.x = getDoubleFromBytes(&dataToCmdVel[24]);
    msg.angular.y = getDoubleFromBytes(&dataToCmdVel[32]);
    msg.angular.z = getDoubleFromBytes(&dataToCmdVel[40]);
    toCmdVelPub.publish(msg);
  }

  //отправка пакета в топик "cmd_rot"
  void sendMsgToCamRot(){
    geometry_msgs::Twist msg;
    msg.linear.x  = getDoubleFromBytes(&dataToCamRot[0]);
    msg.linear.y  = getDoubleFromBytes(&dataToCamRot[8]);
    msg.linear.z  = getDoubleFromBytes(&dataToCamRot[16]);
    msg.angular.x = getDoubleFromBytes(&dataToCamRot[24]);
    msg.angular.y = getDoubleFromBytes(&dataToCamRot[32]);
    msg.angular.z = getDoubleFromBytes(&dataToCamRot[40]);
    toCamRotPub.publish(msg);
  }

  //отправка пакета в топик "fire"
  void sendMsgToCmdFire(){
    std_msgs::Bool msg;
    msg.data = static_cast<bool>(cmd_fire);
    toCmdFirePub.publish(msg);
  }

  // отправка полученных данных с ROS-топикa от Tof Cam Control пользователю по протоколу TCP
  void sendMsgToTCP(){
    memcpy(&dataToTCP[0], dataFromReserved, sizeof(dataFromReserved));
    memcpy(&dataToTCP[sizeof(dataFromReserved)], currentState.keepalive, sizeof(currentState.keepalive));
    async_write();
  }

  void handle_write(const boost::system::error_code& error,
      size_t bytes_transferred)
  {
    if (!error && bytes_transferred > 0)
    {
      send_count_TCP++;
      printSendToTCPData(bytes_transferred);
    }
    else
    {
      std::cout << error.message() << "\n";
      delete this;
    }
  }
};

class TCPServer{
public:
  TCPServer(boost::asio::io_context& io_context, int tcp_port_general)
  : io_context(io_context), tcp_port_general_(tcp_port_general), 
    acceptor(io_context, tcp::endpoint(tcp::v4(), tcp_port_general))
  {
    async_accept();
  }
private:
  boost::asio::io_context& io_context;
  tcp::acceptor acceptor;
  int tcp_port_general_;

  void async_accept()
  {
    Session* new_Session = new Session(io_context);
    acceptor.async_accept(new_Session->socket(),
        boost::bind(&TCPServer::handle_accept, this, new_Session,
        boost::asio::placeholders::error));
  }

  void handle_accept(Session* new_Session,
      const boost::system::error_code& error){
    if (!error){
      new_Session->async_read();
    }
    else {
      delete new_Session;
    }
    async_accept();
  }
};

int main(int argc, char* argv[])
{
  try{
    std::cout << "\n\033[1;32m╔═══════════════════════════════╗\033[0m"
              << "\n\033[1;32m║      eth_rx_tx is running!    ║\033[0m" 
              << "\n\033[1;32m╚═══════════════════════════════╝\033[0m\n";
    ros::init(argc, argv, "eth_rx_tx");
    int tcp_port_general = 20002;
    ros::param::get("/_tcp_port_general", tcp_port_general);
    std::cout << "port: " << tcp_port_general << "\n";

    boost::asio::io_context io_context;
    TCPServer tcpServer(io_context, tcp_port_general);
    ros::Rate rate(10);
    while(ros::ok()){
      io_context.poll_one();
      ros::spinOnce();
      rate.sleep();
    }
  } catch (std::exception e){
    std::cerr << "Exeption: " << e.what() << std::endl;
  }
  return 0;
}
