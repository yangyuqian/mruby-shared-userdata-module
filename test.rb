begin
  a_foo = Foo.new('init value')
rescue Exception => e
  Nginx.echo e
end

