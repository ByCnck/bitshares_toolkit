#include <bts/rpc/rpc_server.hpp>
#include <fc/reflect/variant.hpp>
#include <fc/network/tcp_socket.hpp>
#include <fc/rpc/json_connection.hpp>
#include <fc/thread/thread.hpp>
#include <boost/bind.hpp>

namespace bts { namespace rpc { 

  namespace detail 
  {
    class rpc_server_impl
    {
       public:
         rpc_server::config  _config;
         client_ptr          _client;
         fc::tcp_server      _tcp_serv;
         fc::future<void>    _accept_loop_complete;
         rpc_server*         _self;

         typedef fc::variant (rpc_server_impl::*json_api_method_type)(fc::rpc::json_connection* json_connection, const fc::variants& params);
         typedef std::unordered_map<std::string, json_api_method_type> method_map_type;
         method_map_type _method_map;

         /** the set of connections that have successfully logged in */
         std::unordered_set<fc::rpc::json_connection*> _authenticated_connection_set;

         rpc_server_impl();

         void accept_loop()
         {
           while( !_accept_loop_complete.canceled() )
           {
              fc::tcp_socket_ptr sock = std::make_shared<fc::tcp_socket>();
              try 
              {
                _tcp_serv.accept( *sock );
              }
              catch ( const fc::exception& e )
              {
                elog( "fatal: error opening socket for rpc connection: ${e}", ("e", e.to_detail_string() ) );
                continue;
              }

              auto buf_istream = std::make_shared<fc::buffered_istream>( sock );
              auto buf_ostream = std::make_shared<fc::buffered_ostream>( sock );

              auto json_con = std::make_shared<fc::rpc::json_connection>( std::move(buf_istream), 
                                                                          std::move(buf_ostream) );
              register_methods( json_con );

         //   TODO  0.5 BTC: handle connection errors and and connection closed without
         //   creating an entirely new context... this is waistful
         //     json_con->exec(); 
              fc::async( [json_con]{ json_con->exec().wait(); } );
           }
         }

         void register_methods( const fc::rpc::json_connection_ptr& con )
         {
            ilog( "login!" );
            fc::rpc::json_connection* capture_con = con.get(); 

            for (const method_map_type::value_type& method : _method_map)
            {
              con->add_method(method.first, boost::bind(method.second, this, capture_con, _1));
            }

            _self->register_methods( con );
         } // register methods

        // This method invokes the function directly, called by the CLI intepreter.
        fc::variant direct_invoke_method(const std::string& method_name, const fc::variants& arguments)
        {
          auto iter = _method_map.find(method_name);
          if (iter == _method_map.end())
            FC_THROW_EXCEPTION(exception, "Invalid command ${command}", ("command", method_name));
          return (this->*iter->second)(nullptr, arguments);
        }

        void check_connected_to_network()
        {
          if (!_client->is_connected())
            throw rpc_client_not_connected_exception(FC_LOG_MESSAGE(error, "The client must be connected to the network to execute this command"));
        }

        void check_json_connection_authenticated( fc::rpc::json_connection* con )
        {
          if( con && _authenticated_connection_set.find( con ) == _authenticated_connection_set.end() )
          {
            FC_THROW_EXCEPTION( exception, "not logged in" ); 
          }
        }

        void check_wallet_unlocked()
        {
          if (_client->get_wallet()->is_locked())
            throw rpc_wallet_unlock_needed_exception(FC_LOG_MESSAGE(error, "The wallet's spending key must be unlocked before executing this command"));
        }

        void check_wallet_is_open()
        {
          if (!_client->get_wallet()->is_open())
            throw rpc_wallet_open_needed_exception(FC_LOG_MESSAGE(error, "The wallet must be open before executing this command"));
        }

        fc::variant login(fc::rpc::json_connection* json_connection, const fc::variants& params)
        {
          FC_ASSERT( params.size() == 2 );
          FC_ASSERT( params[0].as_string() == _config.rpc_user )
          FC_ASSERT( params[1].as_string() == _config.rpc_password )
          _authenticated_connection_set.insert( json_connection );
          return fc::variant( true );
        }
        fc::variant openwallet(fc::rpc::json_connection* json_connection, const fc::variants& params)
        {
          check_json_connection_authenticated(json_connection);
          FC_ASSERT(params.size() == 0 || params.size() == 1);
          std::string passphrase;
          if (params.size() == 1)
            passphrase = params[0].as_string();
          try
          {
            _client->get_wallet()->open(_client->get_wallet()->get_wallet_file(), passphrase);
            return fc::variant(true);
          }
          catch (...)
          {
            throw rpc_wallet_passphrase_incorrect_exception();
          }          
        }
        fc::variant walletpassphrase(fc::rpc::json_connection* json_connection, const fc::variants& params)
        {
          check_json_connection_authenticated(json_connection);
          FC_ASSERT(params.size() == 1);
          std::string passphrase = params[0].as_string();
          try
          {
            _client->get_wallet()->unlock_wallet(passphrase);
            return fc::variant(true);
          }
          catch (...)
          {
            throw rpc_wallet_passphrase_incorrect_exception();
          }          
        }
        fc::variant getnewaddress(fc::rpc::json_connection* json_connection, const fc::variants& params)
        {
          check_json_connection_authenticated(json_connection);
          FC_ASSERT(params.size() == 0 || params.size() == 1);
          std::string account;
          if (params.size() == 1)
            account = params[0].as_string();
          bts::blockchain::address new_address = _client->get_wallet()->new_recv_address(account);
          return fc::variant(new_address);
        }
        fc::variant sendtoaddress(fc::rpc::json_connection* json_connection, const fc::variants& params)
        {
          check_json_connection_authenticated(json_connection);
          check_connected_to_network();
          check_wallet_unlocked();
          FC_ASSERT( params.size() >= 2 || params.size() <= 4 );
          bts::blockchain::address destination_address = params[0].as<bts::blockchain::address>();
          bts::blockchain::asset amount = params[1].as<bts::blockchain::asset>();
          // TODO: we're currently ignoring optional parameters 3 and 4,  [comment]a and [to-comment]
          bts::blockchain::signed_transaction trx = _client->get_wallet()->transfer(amount, destination_address);
          _client->broadcast_transaction(trx);
          return fc::variant( trx.id() ); 
        }
        fc::variant listrecvaddresses(fc::rpc::json_connection* json_connection, const fc::variants& params)
        {
          check_json_connection_authenticated(json_connection);
          FC_ASSERT( params.size() == 0 );
          std::unordered_map<bts::blockchain::address,std::string> addresses = _client->get_wallet()->get_recv_addresses();
          return fc::variant( addresses ); 
        }
        fc::variant getbalance(fc::rpc::json_connection* json_connection, const fc::variants& params)
        {
          check_json_connection_authenticated(json_connection);
          FC_ASSERT( params.size() == 0 || params.size() == 1 );
          bts::blockchain::asset_type unit = 0;
          if (params.size() == 1)
            unit = params[0].as<bts::blockchain::asset_type>();
          return fc::variant( _client->get_wallet()->get_balance( unit ) ); 
        }
        fc::variant get_transaction(fc::rpc::json_connection* json_connection, const fc::variants& params)
        {
          check_json_connection_authenticated(json_connection);
          FC_ASSERT( params.size() == 1 );
          return fc::variant( _client->get_chain()->fetch_transaction( params[0].as<transaction_id_type>() )  ); 
        }
        fc::variant getblock(fc::rpc::json_connection* json_connection, const fc::variants& params)
        {
          check_json_connection_authenticated(json_connection);
          FC_ASSERT( params.size() == 1 );
          return fc::variant( _client->get_chain()->fetch_block( (uint32_t)params[0].as_int64() )  ); 
        }
        fc::variant validateaddress(fc::rpc::json_connection* json_connection, const fc::variants& params)
        {
          check_json_connection_authenticated(json_connection);
          FC_ASSERT( params.size() == 1 );
          try {
              return fc::variant(params[0].as_string());
          } 
          catch ( const fc::exception& )
          {
            return fc::variant(false);
          }
        }
        fc::variant rescan(fc::rpc::json_connection* json_connection, const fc::variants& params)
        {
          check_json_connection_authenticated(json_connection);
          FC_ASSERT( params.size() == 1 );
          uint32_t block_num = (uint32_t)params[0].as_int64();
          _client->get_wallet()->scan_chain(*_client->get_chain(), block_num);
          return fc::variant(true); 
        }
        fc::variant import_bitcoin_wallet(fc::rpc::json_connection* json_connection, const fc::variants& params)
        {
          check_json_connection_authenticated(json_connection);
          FC_ASSERT( params.size() == 2 );
          auto wallet_dat      = params[0].as<fc::path>();
          auto wallet_password = params[1].as_string();
          _client->get_wallet()->import_bitcoin_wallet( wallet_dat, wallet_password );
          return fc::variant(true);
        }
        fc::variant import_private_key(fc::rpc::json_connection* json_connection, const fc::variants& params)
        {
          check_json_connection_authenticated(json_connection);
          FC_ASSERT( params.size() == 1 );
          fc::sha256 hash(params[0].as_string());
          fc::ecc::private_key privkey = fc::ecc::private_key::regenerate(hash);
          _client->get_wallet()->import_key(privkey);
          _client->get_wallet()->save();
          return fc::variant(true);
        }
    };
    rpc_server_impl::rpc_server_impl()
    {
      _method_map = method_map_type{ 
        {"login",                 &rpc_server_impl::login},
        {"openwallet",            &rpc_server_impl::openwallet},
        {"walletpassphrase",      &rpc_server_impl::walletpassphrase},
        {"getnewaddress",         &rpc_server_impl::getnewaddress},
        {"sendtoaddress",         &rpc_server_impl::sendtoaddress},
        {"listrecvaddresses",     &rpc_server_impl::listrecvaddresses},
        {"getbalance",            &rpc_server_impl::getbalance},
        {"get_transaction",       &rpc_server_impl::get_transaction},
        {"getblock",              &rpc_server_impl::getblock},
        {"validateaddress",       &rpc_server_impl::validateaddress},
        {"rescan",                &rpc_server_impl::rescan},
        {"import_bitcoin_wallet", &rpc_server_impl::import_bitcoin_wallet},
        {"import_private_key",    &rpc_server_impl::import_private_key}
      };
    }

  } // detail

  bool rpc_server::config::is_valid() const
  {
    if (rpc_user.empty())
      return false;
    if (rpc_password.empty())
      return false;
    if (!rpc_endpoint.port())
      return false;
    return true;
  }


  rpc_server::rpc_server()
  :my( new detail::rpc_server_impl() )
  {
     my->_self = this;
  }

  rpc_server::~rpc_server()
  { 
     try {
         my->_tcp_serv.close();
         if( my->_accept_loop_complete.valid() )
         {
            my->_accept_loop_complete.cancel();
            my->_accept_loop_complete.wait();
         }
     } 
     catch ( const fc::canceled_exception& ){}
     catch ( const fc::exception& e )
     {
        wlog( "unhandled exception thrown in destructor.\n${e}", ("e", e.to_detail_string() ) );
     }
  }

  client_ptr rpc_server::get_client()const
  {
     return my->_client;
  }

  void rpc_server::set_client( const client_ptr& c )
  {
     my->_client = c;
  }

  void rpc_server::configure( const rpc_server::config& cfg )
  {
    if (!cfg.is_valid())
      return;
     try {
       my->_config = cfg;
       my->_tcp_serv.listen( cfg.rpc_endpoint );
       ilog( "listening for rpc connections on port ${port}", ("port",my->_tcp_serv.get_port()) );
     
       my->_accept_loop_complete = fc::async( [=]{ my->accept_loop(); } );
     
     } FC_RETHROW_EXCEPTIONS( warn, "attempting to configure rpc server ${port}", ("port",cfg.rpc_endpoint)("config",cfg) );
  }

  fc::variant rpc_server::direct_invoke_method(const std::string& method_name, const fc::variants& arguments)
  {
    return my->direct_invoke_method(method_name, arguments);
  }
  /**
   *  Adds the JSON-RPC methods to the connection.  This method should be overridden to add 
   *  additional methods for customized DACs.
   */
  void rpc_server::register_methods( const fc::rpc::json_connection_ptr& con )
  {
  }

  void rpc_server::check_json_connection_authenticated( fc::rpc::json_connection* con )
  {
     my->check_json_connection_authenticated( con );
  }

#define RPC_EXCEPTION_IMPL(TYPE, ERROR_CODE, DESCRIPTION) \
  TYPE::TYPE(fc::log_message&& m) : \
    fc::exception(fc::move(m)) {} \
  TYPE::TYPE(){} \
  TYPE::TYPE(const TYPE& t) : fc::exception(t) {} \
  TYPE::TYPE(fc::log_messages m) : \
    fc::exception() {} \
  const char* TYPE::what() const throw() { return DESCRIPTION; } \
  int32_t TYPE::get_rpc_error_code() const { return ERROR_CODE; }

// the codes here match bitcoine error codes in https://github.com/bitcoin/bitcoin/blob/master/src/rpcprotocol.h#L34
RPC_EXCEPTION_IMPL(rpc_client_not_connected_exception, -9, "The client is not connected to the network")
RPC_EXCEPTION_IMPL(rpc_wallet_unlock_needed_exception, -13, "The wallet's spending key must be unlocked before executing this command")
RPC_EXCEPTION_IMPL(rpc_wallet_passphrase_incorrect_exception, -14, "The wallet passphrase entered was incorrect")

// these error codes don't match anything in bitcoin
RPC_EXCEPTION_IMPL(rpc_wallet_open_needed_exception, -100, "The wallet must be opened before executing this command")

} } // bts::rpc

